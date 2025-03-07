/* This file is part of VoltDB.
 * Copyright (C) 2008-2016 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sstream>
#include <cassert>
#include <cstdio>
#include <algorithm>    // std::find
#include <boost/foreach.hpp>
#include <boost/scoped_ptr.hpp>
#include "storage/persistenttable.h"

#include "common/debuglog.h"
#include "common/serializeio.h"
#include "common/FailureInjection.h"
#include "common/tabletuple.h"
#include "common/UndoQuantum.h"
#include "common/executorcontext.hpp"
#include "common/FatalException.hpp"
#include "common/types.h"
#include "common/RecoveryProtoMessage.h"
#include "common/StreamPredicateList.h"
#include "common/ValueFactory.hpp"
#include "catalog/catalog.h"
#include "catalog/database.h"
#include "catalog/table.h"
#include "catalog/materializedviewinfo.h"
#include "crc/crc32c.h"
#include "indexes/tableindex.h"
#include "indexes/tableindexfactory.h"
#include "logging/LogManager.h"
#include "storage/tableiterator.h"
#include "storage/tablefactory.h"
#include "storage/TableCatalogDelegate.hpp"
#include "storage/PersistentTableStats.h"
#include "storage/PersistentTableUndoInsertAction.h"
#include "storage/PersistentTableUndoDeleteAction.h"
#include "storage/PersistentTableUndoTruncateTableAction.h"
#include "storage/PersistentTableUndoUpdateAction.h"
#include "storage/ConstraintFailureException.h"
#include "storage/TupleStreamException.h"
#include "storage/CopyOnWriteContext.h"
#include "storage/MaterializedViewMetadata.h"
#include "storage/AbstractDRTupleStream.h"

namespace voltdb {

void* keyTupleStorage = NULL;
TableTuple keyTuple;

#define TABLE_BLOCKSIZE 2097152

class SetAndRestorePendingDeleteFlag
{
public:
    SetAndRestorePendingDeleteFlag(TableTuple &target) : m_target(target)
    {
        assert(!m_target.isPendingDelete());
        m_target.setPendingDeleteTrue();
    }
    ~SetAndRestorePendingDeleteFlag()
    {
        m_target.setPendingDeleteFalse();
    }

private:
    TableTuple &m_target;
};

PersistentTable::PersistentTable(int partitionColumn, char * signature, bool isMaterialized, int tableAllocationTargetSize, int tupleLimit, bool drEnabled) :
    Table(tableAllocationTargetSize == 0 ? TABLE_BLOCKSIZE : tableAllocationTargetSize),
    m_iter(this),
    m_allowNulls(),
    m_partitionColumn(partitionColumn),
    m_tupleLimit(tupleLimit),
    m_purgeExecutorVector(),
    stats_(this),
    m_failedCompactionCount(0),
    m_invisibleTuplesPendingDeleteCount(0),
    m_surgeon(*this),
    m_isMaterialized(isMaterialized),
    m_drEnabled(drEnabled),
    m_noAvailableUniqueIndex(false),
    m_smallestUniqueIndex(NULL),
    m_smallestUniqueIndexCrc(0),
    m_drTimestampColumnIndex(-1)
{
    // this happens here because m_data might not be initialized above
    m_iter.reset(m_data.begin());

    for (int ii = 0; ii < TUPLE_BLOCK_NUM_BUCKETS; ii++) {
        m_blocksNotPendingSnapshotLoad.push_back(TBBucketPtr(new TBBucket()));
        m_blocksPendingSnapshotLoad.push_back(TBBucketPtr(new TBBucket()));
    }

    m_preTruncateTable = NULL;
    ::memcpy(&m_signature, signature, 20);
}

void PersistentTable::initializeWithColumns(TupleSchema *schema,
                                            const std::vector<std::string> &columnNames,
                                            bool ownsTupleSchema,
                                            int32_t compactionThreshold)
{
    assert (schema != NULL);
    uint16_t hiddenColumnCount = schema->hiddenColumnCount();
    if (hiddenColumnCount == 1) {
        m_drTimestampColumnIndex = 0; // The first hidden column

        // At some point if we have more than one hidden column int a table,
        // we'll need a system for keeping track of which are which.
    }
    else {
        assert (hiddenColumnCount == 0);
    }

    Table::initializeWithColumns(schema, columnNames, ownsTupleSchema, compactionThreshold);
}

PersistentTable::~PersistentTable()
{
    for (int ii = 0; ii < TUPLE_BLOCK_NUM_BUCKETS; ii++) {
        m_blocksNotPendingSnapshotLoad[ii]->clear();
        m_blocksPendingSnapshotLoad[ii]->clear();
    }

    // delete all tuples to free strings
    TableIterator ti(this, m_data.begin());
    TableTuple tuple(m_schema);
    while (ti.next(tuple)) {
        tuple.freeObjectColumns();
        tuple.setActiveFalse();
    }

    // note this class has ownership of the views, even if they
    // were allocated by VoltDBEngine
    for (int i = 0; i < m_views.size(); i++) {
        delete m_views[i];
    }

    // Indexes are deleted in parent class Table destructor.
}

// ------------------------------------------------------------------
// OPERATIONS
// ------------------------------------------------------------------
void PersistentTable::nextFreeTuple(TableTuple *tuple) {
    // First check whether we have any in our list
    // In the memcheck it uses the heap instead of a free list to help Valgrind.
    if (!m_blocksWithSpace.empty()) {
        VOLT_TRACE("GRABBED FREE TUPLE!\n");
        stx::btree_set<TBPtr >::iterator begin = m_blocksWithSpace.begin();
        TBPtr block = (*begin);
        std::pair<char*, int> retval = block->nextFreeTuple();

        /**
         * Check to see if the block needs to move to a new bucket
         */
        if (retval.second != NO_NEW_BUCKET_INDEX) {
            //Check if if the block is currently pending snapshot
            if (m_blocksNotPendingSnapshot.find(block) != m_blocksNotPendingSnapshot.end()) {
                block->swapToBucket(m_blocksNotPendingSnapshotLoad[retval.second]);
            //Check if the block goes into the pending snapshot set of buckets
            } else if (m_blocksPendingSnapshot.find(block) != m_blocksPendingSnapshot.end()) {
                block->swapToBucket(m_blocksPendingSnapshotLoad[retval.second]);
            } else {
                //In this case the block is actively being snapshotted and isn't eligible for merge operations at all
                //do nothing, once the block is finished by the iterator, the iterator will return it
            }
        }

        tuple->move(retval.first);
        ++m_tupleCount;
        if (!block->hasFreeTuples()) {
            m_blocksWithSpace.erase(block);
        }
        assert (m_columnCount == tuple->sizeInValues());
        return;
    }

    // if there are no tuples free, we need to grab another chunk of memory
    // Allocate a new set of tuples
    TBPtr block = allocateNextBlock();

    // get free tuple
    assert (m_columnCount == tuple->sizeInValues());

    std::pair<char*, int> retval = block->nextFreeTuple();

    /**
     * Check to see if the block needs to move to a new bucket
     */
    if (retval.second != NO_NEW_BUCKET_INDEX) {
        //Check if the block goes into the pending snapshot set of buckets
        if (m_blocksPendingSnapshot.find(block) != m_blocksPendingSnapshot.end()) {
            //std::cout << "Swapping block to nonsnapshot bucket " << static_cast<void*>(block.get()) << " to bucket " << retval.second << std::endl;
            block->swapToBucket(m_blocksPendingSnapshotLoad[retval.second]);
        //Now check if it goes in with the others
        } else if (m_blocksNotPendingSnapshot.find(block) != m_blocksNotPendingSnapshot.end()) {
            //std::cout << "Swapping block to snapshot bucket " << static_cast<void*>(block.get()) << " to bucket " << retval.second << std::endl;
            block->swapToBucket(m_blocksNotPendingSnapshotLoad[retval.second]);
        } else {
            //In this case the block is actively being snapshotted and isn't eligible for merge operations at all
            //do nothing, once the block is finished by the iterator, the iterator will return it
        }
    }

    tuple->move(retval.first);
    ++m_tupleCount;
    if (block->hasFreeTuples()) {
        m_blocksWithSpace.insert(block);
    }
}

void PersistentTable::deleteAllTuples(bool freeAllocatedStrings) {
    // nothing interesting
    TableIterator ti(this, m_data.begin());
    TableTuple tuple(m_schema);
    while (ti.next(tuple)) {
        deleteTuple(tuple, true);
    }
}

void PersistentTable::truncateTableForUndo(VoltDBEngine * engine, TableCatalogDelegate * tcd,
        PersistentTable *originalTable) {
    VOLT_DEBUG("**** Truncate table undo *****\n");

    if (originalTable->m_tableStreamer != NULL) {
        // Elastic Index may complete when undo Truncate
        this->unsetPreTruncateTable();
    }

    std::vector<MaterializedViewMetadata *> views = originalTable->views();
    // reset all view table pointers
    BOOST_FOREACH(MaterializedViewMetadata * originalView, views) {
        PersistentTable * targetTable = originalView->targetTable();
        TableCatalogDelegate * targetTcd =  engine->getTableDelegate(targetTable->name());
        // call decrement reference count on the newly constructed view table
        targetTcd->deleteCommand();
        // update the view table pointer with the original view
        targetTcd->setTable(targetTable);
    }
    this->decrementRefcount();

    // reset base table pointer
    tcd->setTable(originalTable);

    engine->rebuildTableCollections();
}

void PersistentTable::truncateTableRelease(PersistentTable *originalTable) {
    VOLT_DEBUG("**** Truncate table release *****\n");
    m_tuplesPinnedByUndo = 0;
    m_invisibleTuplesPendingDeleteCount = 0;

    if (originalTable->m_tableStreamer != NULL) {
        std::stringstream message;
        message << "Transfering table stream after truncation of table ";
        message << name() << " partition " << originalTable->m_tableStreamer->getPartitionID() << '\n';
        std::string str = message.str();
        LogManager::getThreadLogger(LOGGERID_HOST)->log(voltdb::LOGLEVEL_INFO, &str);

        originalTable->m_tableStreamer->cloneForTruncatedTable(m_surgeon);

        this->unsetPreTruncateTable();
    }

    std::vector<MaterializedViewMetadata *> views = originalTable->views();
    // reset all view table pointers
    BOOST_FOREACH(MaterializedViewMetadata * originalView, views) {
        PersistentTable * targetTable = originalView->targetTable();
        targetTable->decrementRefcount();
    }
    originalTable->decrementRefcount();
}


void PersistentTable::truncateTable(VoltDBEngine* engine, bool fallible) {
    if (isPersistentTableEmpty() == true) {
        return;
    }

    // If the table has only one tuple-storage block, it may be better to truncate
    // table by iteratively deleting table rows. Evalute if this is the case
    // based on the block and tuple block load factor
    if (m_data.size() == 1) {
        // threshold cutoff in terms of block load factor at which truncate is
        // better than tuple-by-tuple delete. Cut-off values are based on worst
        // case scenarios with intent to improve performance and to avoid
        // performance regression by not getting too greedy for performance -
        // in here cut-off have been lowered to favor truncate instead of
        // tuple-by-tuple delete. Cut-off numbers were obtained from benchmark
        // tests performing inserts and truncate under different scenarios outline
        // and comparing them for deleting all rows with a predicate that's always
        // true. Following are scenarios based on which cut-off were obtained:
        // - varying table schema - effect of tables having more columns
        // - varying number of views on table
        // - tables with more varchar columns with size below and above 16
        // - tables with indexes

        // cut-off for table with no views
        const double tableLFCutoffForTrunc = 0.105666;
        //cut-off for table with views
        const double tableWithViewsLFCutoffForTrunc = 0.015416;

        const double blockLoadFactor = m_data.begin().data()->loadFactor();
        if ((blockLoadFactor <= tableLFCutoffForTrunc) ||
            (m_views.size() > 0 && blockLoadFactor <= tableWithViewsLFCutoffForTrunc)) {
            return deleteAllTuples(true);
        }
    }

    TableCatalogDelegate * tcd = engine->getTableDelegate(m_name);
    assert(tcd);

    catalog::Table *catalogTable = engine->getCatalogTable(m_name);
    if (tcd->init(*engine->getDatabase(), *catalogTable) != 0) {
        VOLT_ERROR("Failed to initialize table '%s' from catalog",m_name.c_str());
        return ;
    }

    assert(!tcd->exportEnabled());
    PersistentTable * emptyTable = tcd->getPersistentTable();
    assert(emptyTable);
    assert(emptyTable->views().size() == 0);
    if (m_tableStreamer != NULL && m_tableStreamer->hasStreamType(TABLE_STREAM_ELASTIC_INDEX)) {
        // There is an Elastic Index work going on and it should continue access the old table.
        // Add one reference count to keep the original table.
        emptyTable->setPreTruncateTable(this);
    }

    // add matView
    BOOST_FOREACH(MaterializedViewMetadata * originalView, m_views) {
        PersistentTable * targetTable = originalView->targetTable();
        TableCatalogDelegate * targetTcd =  engine->getTableDelegate(targetTable->name());
        catalog::Table *catalogViewTable = engine->getCatalogTable(targetTable->name());

        if (targetTcd->init(*engine->getDatabase(), *catalogViewTable) != 0) {
            VOLT_ERROR("Failed to initialize table '%s' from catalog",targetTable->name().c_str());
            return ;
        }
        PersistentTable * targetEmptyTable = targetTcd->getPersistentTable();
        assert(targetEmptyTable);
        new MaterializedViewMetadata(emptyTable, targetEmptyTable, originalView->getMaterializedViewInfo());
    }

    // If there is a purge fragment on the old table, pass it on to the new one
    if (hasPurgeFragment()) {
        assert(! emptyTable->hasPurgeFragment());
        boost::shared_ptr<ExecutorVector> evPtr = getPurgeExecutorVector();
        emptyTable->swapPurgeExecutorVector(evPtr);
    }

    engine->rebuildTableCollections();

    ExecutorContext *ec = ExecutorContext::getExecutorContext();
    AbstractDRTupleStream *drStream = getDRTupleStream(ec);
    size_t drMark = INVALID_DR_MARK;
    if (drStream && !m_isMaterialized && m_drEnabled) {
        const int64_t lastCommittedSpHandle = ec->lastCommittedSpHandle();
        const int64_t currentTxnId = ec->currentTxnId();
        const int64_t currentSpHandle = ec->currentSpHandle();
        const int64_t currentUniqueId = ec->currentUniqueId();
        drMark = drStream->truncateTable(lastCommittedSpHandle, m_signature, m_name, currentTxnId, currentSpHandle, currentUniqueId);
    }

    UndoQuantum *uq = ExecutorContext::currentUndoQuantum();
    if (uq) {
        if (!fallible) {
            throwFatalException("Attempted to truncate table %s when there was an "
                                "active undo quantum, and presumably an active transaction that should be there",
                                m_name.c_str());
        }
        emptyTable->m_tuplesPinnedByUndo = emptyTable->m_tupleCount;
        emptyTable->m_invisibleTuplesPendingDeleteCount = emptyTable->m_tupleCount;
        // Create and register an undo action.
        uq->registerUndoAction(new (*uq) PersistentTableUndoTruncateTableAction(engine, tcd, this, emptyTable, &m_surgeon, drMark));
    } else {
        if (fallible) {
            throwFatalException("Attempted to truncate table %s when there was no "
                                "active undo quantum even though one was expected", m_name.c_str());
        }

        //Skip the undo log and "commit" immediately by asking the new emptyTable to perform
        //the truncate table release work rather then having it invoked by PersistentTableUndoTruncateTableAction
        emptyTable->truncateTableRelease(this);
    }
}


void setSearchKeyFromTuple(TableTuple &source) {
    keyTuple.setNValue(0, source.getNValue(1));
    keyTuple.setNValue(1, source.getNValue(2));
}

void PersistentTable::setDRTimestampForTuple(ExecutorContext* ec, TableTuple& tuple, bool update) {
    assert(hasDRTimestampColumn());
    if (update || tuple.getHiddenNValue(getDRTimestampColumnIndex()).isNull()) {
        const int64_t drTimestamp = ec->currentDRTimestamp();
        tuple.setHiddenNValue(getDRTimestampColumnIndex(), ValueFactory::getBigIntValue(drTimestamp));
    }
}

/*
 * Regular tuple insertion that does an allocation and copy for
 * uninlined strings and creates and registers an UndoAction.
 */
bool PersistentTable::insertTuple(TableTuple &source)
{
    insertPersistentTuple(source, true);
    return true;
}

void PersistentTable::insertPersistentTuple(TableTuple &source, bool fallible)
{

    if (fallible && visibleTupleCount() >= m_tupleLimit) {
        char buffer [256];
        snprintf (buffer, 256, "Table %s exceeds table maximum row count %d",
                m_name.c_str(), m_tupleLimit);
        throw ConstraintFailureException(this, source, buffer);
    }

    //
    // First get the next free tuple
    // This will either give us one from the free slot list, or
    // grab a tuple at the end of our chunk of memory
    //
    TableTuple target(m_schema);
    PersistentTable::nextFreeTuple(&target);

    //
    // Then copy the source into the target
    //
    target.copyForPersistentInsert(source); // tuple in freelist must be already cleared

    try {
        insertTupleCommon(source, target, fallible);
    } catch (ConstraintFailureException &e) {
        deleteTupleStorage(target); // also frees object columns
        throw;
    } catch (TupleStreamException &e) {
        deleteTupleStorage(target); // also frees object columns
        throw;
    }
}

void PersistentTable::insertTupleCommon(TableTuple &source, TableTuple &target, bool fallible, bool shouldDRStream)
{
    if (fallible) {
        // not null checks at first
        FAIL_IF(!checkNulls(target)) {
            throw ConstraintFailureException(this, source, TableTuple(), CONSTRAINT_TYPE_NOT_NULL);
        }

    }

    // Write to DR stream before everything else to ensure nothing gets left in
    // the index if the append fails.
    ExecutorContext *ec = ExecutorContext::getExecutorContext();
    if (hasDRTimestampColumn()) {
        setDRTimestampForTuple(ec, target, false);
    }

    AbstractDRTupleStream *drStream = getDRTupleStream(ec);
    size_t drMark = INVALID_DR_MARK;
    if (drStream && !m_isMaterialized && m_drEnabled && shouldDRStream) {
        ExecutorContext *ec = ExecutorContext::getExecutorContext();
        const int64_t lastCommittedSpHandle = ec->lastCommittedSpHandle();
        const int64_t currentTxnId = ec->currentTxnId();
        const int64_t currentSpHandle = ec->currentSpHandle();
        const int64_t currentUniqueId = ec->currentUniqueId();
        std::pair<const TableIndex*, uint32_t> uniqueIndex = getUniqueIndexForDR();
        drMark = drStream->appendTuple(lastCommittedSpHandle, m_signature, currentTxnId, currentSpHandle, currentUniqueId, target, DR_RECORD_INSERT, uniqueIndex);
    }

    if (m_schema->getUninlinedObjectColumnCount() != 0) {
        increaseStringMemCount(target.getNonInlinedMemorySize());
    }

    target.setActiveTrue();
    target.setPendingDeleteFalse();
    target.setPendingDeleteOnUndoReleaseFalse();

    /**
     * Inserts never "dirty" a tuple since the tuple is new, but...  The
     * COWIterator may still be scanning and if the tuple came from the free
     * list then it may need to be marked as dirty so it will be skipped. If COW
     * is on have it decide. COW should always set the dirty to false unless the
     * tuple is in a to be scanned area.
     */
    if (m_tableStreamer == NULL || !m_tableStreamer->notifyTupleInsert(target)) {
        target.setDirtyFalse();
    }

    TableTuple conflict(m_schema);
    tryInsertOnAllIndexes(&target, &conflict);
    if (!conflict.isNullTuple()) {
        // Roll the DR stream back because the undo action is not registered
        m_surgeon.DRRollback(drMark, rowCostForDRRecord(DR_RECORD_INSERT));
        throw ConstraintFailureException(this, source, conflict, CONSTRAINT_TYPE_UNIQUE);
    }

    // this is skipped for inserts that are never expected to fail,
    // like some (initially, all) cases of tuple migration on schema change
    if (fallible) {
        /*
         * Create and register an undo action.
         */
        UndoQuantum *uq = ExecutorContext::currentUndoQuantum();
        if (uq) {
            char* tupleData = uq->allocatePooledCopy(target.address(), target.tupleLength());
            //* enable for debug */ std::cout << "DEBUG: inserting " << (void*)target.address()
            //* enable for debug */           << " { " << target.debugNoHeader() << " } "
            //* enable for debug */           << " copied to " << (void*)tupleData << std::endl;
            uq->registerUndoAction(new (*uq) PersistentTableUndoInsertAction(tupleData, &m_surgeon, drMark));
        }
    }

    // handle any materialized views
    for (int i = 0; i < m_views.size(); i++) {
        m_views[i]->processTupleInsert(target, fallible);
    }
}

/*
 * Insert a tuple but don't allocate a new copy of the uninlineable
 * strings or create an UndoAction or update a materialized view.
 */
void PersistentTable::insertTupleForUndo(char *tuple)
{
    TableTuple target(m_schema);
    target.move(tuple);
    target.setPendingDeleteOnUndoReleaseFalse();
    m_tuplesPinnedByUndo--;
    --m_invisibleTuplesPendingDeleteCount;

    /*
     * The only thing to do is reinsert the tuple into the indexes. It was never moved,
     * just marked as deleted.
     */
    TableTuple conflict(m_schema);
    tryInsertOnAllIndexes(&target, &conflict);
    if (!conflict.isNullTuple()) {
        // First off, it should be impossible to violate a constraint when RESTORING an index to a
        // known good state via an UNDO of a delete.  So, assume that something is badly broken, here.
        // It's probably safer NOT to do too much cleanup -- such as trying to call deleteTupleStorage --
        // as there's no guarantee that it will improve things, and is likely just to tamper with
        // the crime scene.
        throwFatalException("Failed to insert tuple into table %s for undo:"
                            " unique constraint violation\n%s\n", m_name.c_str(),
                            target.debugNoHeader().c_str());
    }
}

/*
 * Regular tuple update function that does a copy and allocation for
 * updated strings and creates an UndoAction. Additional optimization
 * for callers that know which indexes to update.
 */
bool PersistentTable::updateTupleWithSpecificIndexes(TableTuple &targetTupleToUpdate,
                                                     TableTuple &sourceTupleWithNewValues,
                                                     std::vector<TableIndex*> const &indexesToUpdate,
                                                     bool fallible,
                                                     bool updateDRTimestamp)
{
    UndoQuantum *uq = NULL;
    char* oldTupleData = NULL;
    int tupleLength = targetTupleToUpdate.tupleLength();
    /**
     * Check for index constraint violations.
     */
    if (fallible) {
        if ( ! checkUpdateOnUniqueIndexes(targetTupleToUpdate,
                                          sourceTupleWithNewValues,
                                          indexesToUpdate)) {
            throw ConstraintFailureException(this,
                                             sourceTupleWithNewValues,
                                             targetTupleToUpdate,
                                             CONSTRAINT_TYPE_UNIQUE);
        }

        /**
         * Check for null constraint violations. Assumes source tuple is fully fleshed out.
         */
        FAIL_IF(!checkNulls(sourceTupleWithNewValues)) {
            throw ConstraintFailureException(this,
                                             sourceTupleWithNewValues,
                                             targetTupleToUpdate,
                                             CONSTRAINT_TYPE_NOT_NULL);
        }

        uq = ExecutorContext::currentUndoQuantum();
        if (uq) {
            /*
             * For undo purposes, before making any changes, save a copy of the state of the tuple
             * into the undo pool temp storage and hold onto it with oldTupleData.
             */
            oldTupleData = uq->allocatePooledCopy(targetTupleToUpdate.address(), targetTupleToUpdate.tupleLength());
        }
    }

    // Write to the DR stream before doing anything else to ensure we don't
    // leave a half updated tuple behind in case this throws.
    ExecutorContext *ec = ExecutorContext::getExecutorContext();
    if (hasDRTimestampColumn() && updateDRTimestamp) {
        setDRTimestampForTuple(ec, sourceTupleWithNewValues, true);
    }

    AbstractDRTupleStream *drStream = getDRTupleStream(ec);
    size_t drMark = INVALID_DR_MARK;
    if (drStream && !m_isMaterialized && m_drEnabled) {
        ExecutorContext *ec = ExecutorContext::getExecutorContext();
        const int64_t lastCommittedSpHandle = ec->lastCommittedSpHandle();
        const int64_t currentTxnId = ec->currentTxnId();
        const int64_t currentSpHandle = ec->currentSpHandle();
        const int64_t currentUniqueId = ec->currentUniqueId();
        std::pair<const TableIndex*, uint32_t> uniqueIndex = getUniqueIndexForDR();
        drMark = drStream->appendUpdateRecord(lastCommittedSpHandle, m_signature, currentTxnId, currentSpHandle, currentUniqueId, targetTupleToUpdate, sourceTupleWithNewValues, uniqueIndex);
    }

    if (m_tableStreamer != NULL) {
        m_tableStreamer->notifyTupleUpdate(targetTupleToUpdate);
    }

    /**
     * Remove the current tuple from any indexes.
     */
    bool someIndexGotUpdated = false;
    bool indexRequiresUpdate[indexesToUpdate.size()];
    if (indexesToUpdate.size()) {
        someIndexGotUpdated = true;
        for (int i = 0; i < indexesToUpdate.size(); i++) {
            TableIndex *index = indexesToUpdate[i];
            if (!index->keyUsesNonInlinedMemory()) {
                if (!index->checkForIndexChange(&targetTupleToUpdate, &sourceTupleWithNewValues)) {
                    indexRequiresUpdate[i] = false;
                    continue;
                }
            }
            indexRequiresUpdate[i] = true;
            if (!index->deleteEntry(&targetTupleToUpdate)) {
                throwFatalException("Failed to remove tuple from index (during update) in Table: %s Index %s",
                                    m_name.c_str(), index->getName().c_str());
            }
        }
    }

    {
        // handle any materialized views, hide the tuple from the scan temporarily.
        SetAndRestorePendingDeleteFlag setPending(targetTupleToUpdate);
        for (int i = 0; i < m_views.size(); i++) {
            m_views[i]->processTupleDelete(targetTupleToUpdate, fallible);
        }
    }

    if (m_schema->getUninlinedObjectColumnCount() != 0) {
        decreaseStringMemCount(targetTupleToUpdate.getNonInlinedMemorySize());
        increaseStringMemCount(sourceTupleWithNewValues.getNonInlinedMemorySize());
    }

    // TODO: This is a little messed up.
    // We know what we want the target tuple's flags to look like after the copy,
    // so we carefully set them (rather than, say, ignore them) on the source tuple
    // and make sure to copy them (rather than, say, ignore them) in copyForPersistentUpdate
    // and that allows us to ignore them (rather than, say, set them) afterwards on the actual
    // target tuple that matters. What could be simpler?
    sourceTupleWithNewValues.setActiveTrue();
    // The isDirty flag is especially interesting because the COWcontext found it more convenient
    // to mark it on the target tuple. So, no problem, just copy it from the target tuple to the
    // source tuple so it can get copied back to the target tuple in copyForPersistentUpdate. Brilliant!
    //Copy the dirty status that was set by markTupleDirty.
    if (targetTupleToUpdate.isDirty()) {
        sourceTupleWithNewValues.setDirtyTrue();
    } else {
        sourceTupleWithNewValues.setDirtyFalse();
    }

    // Either the "before" or "after" object reference values that change will come in handy later,
    // so collect them up.
    std::vector<char*> oldObjects;
    std::vector<char*> newObjects;

    // this is the actual write of the new values
    targetTupleToUpdate.copyForPersistentUpdate(sourceTupleWithNewValues, oldObjects, newObjects);

    if (uq) {
        /*
         * Create and register an undo action with copies of the "before" and "after" tuple storage
         * and the "before" and "after" object pointers for non-inlined columns that changed.
         */
        char* newTupleData = uq->allocatePooledCopy(targetTupleToUpdate.address(), tupleLength);
        uq->registerUndoAction(new (*uq) PersistentTableUndoUpdateAction(oldTupleData, newTupleData,
                                                                         oldObjects, newObjects,
                                                                         &m_surgeon, someIndexGotUpdated,
                                                                         drMark));
    } else {
        // This is normally handled by the Undo Action's release (i.e. when there IS an Undo Action)
        // -- though maybe even that case should delegate memory management back to the PersistentTable
        // to keep the UndoAction stupid simple?
        // Anyway, there is no Undo Action in this case, so DIY.
        NValue::freeObjectsFromTupleStorage(oldObjects);
    }

    /**
     * Insert the updated tuple back into the indexes.
     */
    TableTuple conflict(m_schema);
    for (int i = 0; i < indexesToUpdate.size(); i++) {
        TableIndex *index = indexesToUpdate[i];
        if (!indexRequiresUpdate[i]) {
            continue;
        }
        index->addEntry(&targetTupleToUpdate, &conflict);
        if (!conflict.isNullTuple()) {
            throwFatalException("Failed to insert updated tuple into index in Table: %s Index %s",
                                m_name.c_str(), index->getName().c_str());
        }
    }

    // handle any materialized views
    for (int i = 0; i < m_views.size(); i++) {
        m_views[i]->processTupleInsert(targetTupleToUpdate, fallible);
    }
    return true;
}

/*
 * sourceTupleWithNewValues contains a copy of the tuple data before the update
 * and tupleWithUnwantedValues contains a copy of the updated tuple data.
 * First remove the current tuple value from any indexes (if asked to do so).
 * Then revert the tuple to the original preupdate values by copying the source to the target.
 * Then insert the new (or rather, old) value back into the indexes.
 */
void PersistentTable::updateTupleForUndo(char* tupleWithUnwantedValues,
                                         char* sourceTupleDataWithNewValues,
                                         bool revertIndexes)
{
    TableTuple matchable(m_schema);
    // Get the address of the tuple in the table from one of the copies on hand.
    // Any TableScan OR a primary key lookup on an already updated index will find the tuple
    // by its unwanted updated values.
    if (revertIndexes || primaryKeyIndex() == NULL) {
        matchable.move(tupleWithUnwantedValues);
    }
    // A primary key lookup on a not-yet-updated index will find the tuple by its original/new values.
    else {
        matchable.move(sourceTupleDataWithNewValues);
    }
    TableTuple targetTupleToUpdate = lookupTupleForUndo(matchable);
    TableTuple sourceTupleWithNewValues(sourceTupleDataWithNewValues, m_schema);

    //If the indexes were never updated there is no need to revert them.
    if (revertIndexes) {
        BOOST_FOREACH(TableIndex *index, m_indexes) {
            if (!index->deleteEntry(&targetTupleToUpdate)) {
                throwFatalException("Failed to update tuple in Table: %s Index %s",
                                    m_name.c_str(), index->getName().c_str());
            }
        }
    }

    if (m_schema->getUninlinedObjectColumnCount() != 0)
    {
        decreaseStringMemCount(targetTupleToUpdate.getNonInlinedMemorySize());
        increaseStringMemCount(sourceTupleWithNewValues.getNonInlinedMemorySize());
    }

    bool dirty = targetTupleToUpdate.isDirty();
    // this is the actual in-place revert to the old version
    targetTupleToUpdate.copy(sourceTupleWithNewValues);
    if (dirty) {
        targetTupleToUpdate.setDirtyTrue();
    } else {
        targetTupleToUpdate.setDirtyFalse();
    }

    //If the indexes were never updated there is no need to revert them.
    if (revertIndexes) {
        TableTuple conflict(m_schema);
        BOOST_FOREACH(TableIndex *index, m_indexes) {
            index->addEntry(&targetTupleToUpdate, &conflict);
            if (!conflict.isNullTuple()) {
                throwFatalException("Failed to update tuple in Table: %s Index %s",
                                    m_name.c_str(), index->getName().c_str());
            }
        }
    }
}

bool PersistentTable::deleteTuple(TableTuple &target, bool fallible) {
    // May not delete an already deleted tuple.
    assert(target.isActive());

    // The tempTuple is forever!
    assert(&target != &m_tempTuple);

    // Write to the DR stream before doing anything else to ensure nothing will
    // be left forgotten in case this throws.
    ExecutorContext *ec = ExecutorContext::getExecutorContext();
    AbstractDRTupleStream *drStream = getDRTupleStream(ec);
    size_t drMark = INVALID_DR_MARK;
    if (drStream && !m_isMaterialized && m_drEnabled) {
        const int64_t lastCommittedSpHandle = ec->lastCommittedSpHandle();
        const int64_t currentTxnId = ec->currentTxnId();
        const int64_t currentSpHandle = ec->currentSpHandle();
        const int64_t currentUniqueId = ec->currentUniqueId();
        std::pair<const TableIndex*, uint32_t> uniqueIndex = getUniqueIndexForDR();
        drMark = drStream->appendTuple(lastCommittedSpHandle, m_signature, currentTxnId, currentSpHandle, currentUniqueId, target, DR_RECORD_DELETE, uniqueIndex);
    }

    // Just like insert, we want to remove this tuple from all of our indexes
    deleteFromAllIndexes(&target);

    {
        // handle any materialized views, hide the tuple from the scan temporarily.
        SetAndRestorePendingDeleteFlag setPending(target);
        for (int i = 0; i < m_views.size(); i++) {
            m_views[i]->processTupleDelete(target, fallible);
        }
    }

    if (fallible) {
        UndoQuantum *uq = ExecutorContext::currentUndoQuantum();
        if (uq) {
            target.setPendingDeleteOnUndoReleaseTrue();
            m_tuplesPinnedByUndo++;
            ++m_invisibleTuplesPendingDeleteCount;
            // Create and register an undo action.
            uq->registerUndoAction(new (*uq) PersistentTableUndoDeleteAction(target.address(), &m_surgeon, drMark), this);
            return true;
        }
    }

    // Here, for reasons of infallibility or no active UndoLog, there is no undo, there is only DO.
    deleteTupleFinalize(target);
    return true;
}


/**
 * This entry point is triggered by the successful release of an UndoDeleteAction.
 */
void PersistentTable::deleteTupleRelease(char* tupleData)
{
    TableTuple target(m_schema);
    target.move(tupleData);
    target.setPendingDeleteOnUndoReleaseFalse();
    m_tuplesPinnedByUndo--;
    --m_invisibleTuplesPendingDeleteCount;
    deleteTupleFinalize(target);
}

/**
 * Actually follow through with a "delete" -- this is common code between UndoDeleteAction release and the
 * all-at-once infallible deletes that bypass Undo processing.
 */
void PersistentTable::deleteTupleFinalize(TableTuple &target)
{
    // A snapshot (background scan) in progress can still cause a hold-up.
    // notifyTupleDelete() defaults to returning true for all context types
    // other than CopyOnWriteContext.
    if (   m_tableStreamer != NULL
        && ! m_tableStreamer->notifyTupleDelete(target)) {
        // Mark it pending delete and let the snapshot land the finishing blow.

        // This "already pending delete" guard prevents any
        // (possible?) case of double-counting a doubly-applied pending delete
        // before it gets ignored.
        // This band-aid guard just keeps such a condition from becoming an
        // inconvenience to a "testability feature" implemented in tableutil.cpp
        // for the benefit of CopyOnWriteTest.cpp.
        // Maybe it should just be an assert --
        // maybe we are missing a final opportunity to detect the "inconceivable",
        // which, if ignored, may leave a wake of mysterious and catastrophic side effects.
        // There's always the option of setting a breakpoint on this return.
        if (target.isPendingDelete()) {
            return;
        }

        ++m_invisibleTuplesPendingDeleteCount;
        target.setPendingDeleteTrue();
        return;
    }

    // No snapshot in progress cares, just whack it.
    deleteTupleStorage(target); // also frees object columns
}

/**
 * Assumptions:
 *  All tuples will be deleted in storage order.
 *  Indexes and views have been destroyed first.
 */
void PersistentTable::deleteTupleForSchemaChange(TableTuple &target) {
    deleteTupleStorage(target); // also frees object columns
}

/*
 * Delete a tuple by looking it up via table scan or a primary key
 * index lookup. An undo initiated delete like deleteTupleForUndo
 * is in response to the insertion of a new tuple by insertTuple
 * and that by definition is a tuple that is of no interest to
 * the COWContext. The COWContext set the tuple to have the
 * correct dirty setting when the tuple was originally inserted.
 * TODO remove duplication with regular delete. Also no view updates.
 *
 * NB: This is also used as a generic delete for Elastic rebalance.
 *     skipLookup will be true in this case because the passed tuple
 *     can be used directly.
 */
void PersistentTable::deleteTupleForUndo(char* tupleData, bool skipLookup) {
    TableTuple matchable(tupleData, m_schema);
    TableTuple target(tupleData, m_schema);
    //* enable for debug */ std::cout << "DEBUG: undoing "
    //* enable for debug */           << " { " << target.debugNoHeader() << " } "
    //* enable for debug */           << " copied to " << (void*)tupleData << std::endl;
    if (!skipLookup) {
        // The UndoInsertAction got a pooled copy of the tupleData.
        // Relocate the original tuple actually in the table.
        target = lookupTupleForUndo(matchable);
    }
    if (target.isNullTuple()) {
        throwFatalException("Failed to delete tuple from table %s:"
                            " tuple does not exist\n%s\n", m_name.c_str(),
                            matchable.debugNoHeader().c_str());
    }
    //* enable for debug */ std::cout << "DEBUG: finding " << (void*)target.address()
    //* enable for debug */           << " { " << target.debugNoHeader() << " } "
    //* enable for debug */           << " copied to " << (void*)tupleData << std::endl;

    // Make sure that they are not trying to delete the same tuple twice
    assert(target.isActive());

    deleteFromAllIndexes(&target);
    deleteTupleFinalize(target); // also frees object columns
}

TableTuple PersistentTable::lookupTuple(TableTuple tuple, LookupType lookupType) {
    TableTuple nullTuple(m_schema);

    TableIndex *pkeyIndex = primaryKeyIndex();
    if (pkeyIndex == NULL) {
        /*
         * Do a table scan.
         */
        TableTuple tableTuple(m_schema);
        TableIterator ti(this, m_data.begin());
        if (lookupType == LOOKUP_FOR_UNDO || m_schema->getUninlinedObjectColumnCount() == 0) {
            size_t tuple_length;
            if (lookupType == LOOKUP_BY_VALUES && m_schema->hiddenColumnCount() > 0) {
                // Looking up a tuple by values should not include any internal
                // hidden column values, which are appended to the end of the
                // tuple
                tuple_length = m_schema->offsetOfHiddenColumns();
            } else {
                tuple_length = m_schema->tupleLength();
            }
            // Do an inline tuple byte comparison
            // to avoid matching duplicate tuples with different pointers to Object storage
            // -- which would cause erroneous releases of the wrong Object storage copy.
            while (ti.hasNext()) {
                ti.next(tableTuple);
                char* tableTupleData = tableTuple.address() + TUPLE_HEADER_SIZE;
                char* tupleData = tuple.address() + TUPLE_HEADER_SIZE;
                if (::memcmp(tableTupleData, tupleData, tuple_length) == 0) {
                    return tableTuple;
                }
            }
        } else {
            bool includeHiddenColumns = (lookupType == LOOKUP_FOR_DR);
            while (ti.hasNext()) {
                ti.next(tableTuple);
                if (tableTuple.equalsNoSchemaCheck(tuple, includeHiddenColumns)) {
                    return tableTuple;
                }
            }
        }
        return nullTuple;
    }

    return pkeyIndex->uniqueMatchingTuple(tuple);
}

void PersistentTable::insertIntoAllIndexes(TableTuple *tuple) {
    TableTuple conflict(m_schema);
    BOOST_FOREACH(TableIndex *index, m_indexes) {
        index->addEntry(tuple, &conflict);
        if (!conflict.isNullTuple()) {
            throwFatalException(
                    "Failed to insert tuple in Table: %s Index %s", m_name.c_str(), index->getName().c_str());
        }
    }
}

void PersistentTable::deleteFromAllIndexes(TableTuple *tuple) {
    BOOST_FOREACH(TableIndex *index, m_indexes) {
        if (!index->deleteEntry(tuple)) {
            throwFatalException(
                    "Failed to delete tuple in Table: %s Index %s", m_name.c_str(), index->getName().c_str());
        }
    }
}

void PersistentTable::tryInsertOnAllIndexes(TableTuple *tuple, TableTuple *conflict) {
    for (int i = 0; i < static_cast<int>(m_indexes.size()); ++i) {
        m_indexes[i]->addEntry(tuple, conflict);
        FAIL_IF(!conflict->isNullTuple()) {
            VOLT_DEBUG("Failed to insert into index %s,%s",
                       m_indexes[i]->getTypeName().c_str(),
                       m_indexes[i]->getName().c_str());
            for (int j = 0; j < i; ++j) {
                m_indexes[j]->deleteEntry(tuple);
            }
            return;
        }
    }
}

bool PersistentTable::checkUpdateOnUniqueIndexes(TableTuple &targetTupleToUpdate,
                                                 const TableTuple &sourceTupleWithNewValues,
                                                 std::vector<TableIndex*> const &indexesToUpdate)
{
    BOOST_FOREACH(TableIndex* index, indexesToUpdate) {
        if (index->isUniqueIndex()) {
            if (index->checkForIndexChange(&targetTupleToUpdate, &sourceTupleWithNewValues) == false)
                continue; // no update is needed for this index

            // if there is a change, the new_key has to be checked
            FAIL_IF (index->exists(&sourceTupleWithNewValues)) {
                VOLT_WARN("Unique Index '%s' complained to the update",
                          index->debug().c_str());
                return false; // cannot insert the new value
            }
        }
    }

    return true;
}

bool PersistentTable::checkNulls(TableTuple &tuple) const {
    assert (m_columnCount == tuple.sizeInValues());
    for (int i = m_columnCount - 1; i >= 0; --i) {
        if (( ! m_allowNulls[i]) && tuple.isNull(i)) {
            VOLT_TRACE ("%d th attribute was NULL. It is non-nillable attribute.", i);
            return false;
        }
    }
    return true;
}

/*
 * claim ownership of a view. table is responsible for this view*
 */
void PersistentTable::addMaterializedView(MaterializedViewMetadata *view)
{
    m_views.push_back(view);
}

/*
 * drop a view. the table is no longer feeding it.
 * The destination table will go away when the view metadata is deleted (or later?) as its refcount goes to 0.
 */
void PersistentTable::dropMaterializedView(MaterializedViewMetadata *targetView)
{
    assert( ! m_views.empty());
    MaterializedViewMetadata *lastView = m_views.back();
    if (targetView != lastView) {
        // iterator to vector element:
        std::vector<MaterializedViewMetadata*>::iterator toView = find(m_views.begin(), m_views.end(), targetView);
        assert(toView != m_views.end());
        // Use the last view to patch the potential hole.
        *toView = lastView;
    }
    // The last element is now excess.
    m_views.pop_back();
    delete targetView;
}

void
PersistentTable::segregateMaterializedViews(std::map<std::string, catalog::MaterializedViewInfo*>::const_iterator const & start,
                                            std::map<std::string, catalog::MaterializedViewInfo*>::const_iterator const & end,
                                            std::vector< catalog::MaterializedViewInfo*> &survivingInfosOut,
                                            std::vector<MaterializedViewMetadata*> &survivingViewsOut,
                                            std::vector<MaterializedViewMetadata*> &obsoleteViewsOut)
{
    //////////////////////////////////////////////////////////
    // find all of the materialized views to remove or keep
    //////////////////////////////////////////////////////////

    // iterate through all of the existing views
    BOOST_FOREACH(MaterializedViewMetadata* currView, m_views) {
        std::string currentViewId = currView->targetTable()->name();

        // iterate through all of the catalog views, looking for a match.
        std::map<std::string, catalog::MaterializedViewInfo*>::const_iterator viewIter;
        bool viewfound = false;
        for (viewIter = start; viewIter != end; ++viewIter) {
            catalog::MaterializedViewInfo* catalogViewInfo = viewIter->second;
            if (currentViewId == catalogViewInfo->name()) {
                viewfound = true;
                //TODO: This MIGHT be a good place to identify the need for view re-definition.
                survivingInfosOut.push_back(catalogViewInfo);
                survivingViewsOut.push_back(currView);
                break;
            }
        }

        // if the table has a view that the catalog doesn't, then prepare to remove (or fail to migrate) the view
        if (!viewfound) {
            obsoleteViewsOut.push_back(currView);
        }
    }
}

void
PersistentTable::updateMaterializedViewTargetTable(PersistentTable* target, catalog::MaterializedViewInfo* targetMvInfo)
{
    std::string targetName = target->name();
    // find the materialized view that uses the table or its precursor (by the same name).
    BOOST_FOREACH(MaterializedViewMetadata* currView, m_views) {
        PersistentTable* currTarget = currView->targetTable();

        // found: target is alreafy set
        if (currTarget == target) {
            // The view is already up to date.
            // but still need to update the index used for min/max
            currView->setIndexForMinMax(targetMvInfo->indexForMinMax());
            // Fallback executor vectors must be set after indexForMinMax
            currView->setFallbackExecutorVectors(targetMvInfo->fallbackQueryStmts());
            return;
        }

        // found: this is the table to set the
        std::string currName = currTarget->name();
        if (currName == targetName) {
            // A match on name only indicates that the target table has been re-defined since
            // the view was initialized, so re-initialize the view.
            currView->setTargetTable(target);
            currView->setIndexForMinMax(targetMvInfo->indexForMinMax());
            // Fallback executor vectors must be set after indexForMinMax
            currView->setFallbackExecutorVectors(targetMvInfo->fallbackQueryStmts());
            return;
        }
    }

    // The connection needs to be made using a new MaterializedViewMetadata
    // This is not a leak -- the materialized view is self-installing into srcTable.
    new MaterializedViewMetadata(this, target, targetMvInfo);
}

// ------------------------------------------------------------------
// UTILITY
// ------------------------------------------------------------------
std::string PersistentTable::tableType() const {
    return "PersistentTable";
}

std::string PersistentTable::debug() {
    std::ostringstream buffer;
    buffer << Table::debug();
    buffer << "\tINDEXES: " << m_indexes.size() << "\n";

    // Indexes
    buffer << "===========================================================\n";
    for (int index_ctr = 0; index_ctr < m_indexes.size(); ++index_ctr) {
        if (m_indexes[index_ctr]) {
            buffer << "\t[" << index_ctr << "] " << m_indexes[index_ctr]->debug();
            //
            // Primary Key
            //
            if (m_pkeyIndex != NULL && m_pkeyIndex->getName().compare(m_indexes[index_ctr]->getName()) == 0) {
                buffer << " [PRIMARY KEY]";
            }
            buffer << "\n";
        }
    }

    return buffer.str();
}

void PersistentTable::onSetColumns() {
    m_allowNulls.resize(m_columnCount);
    for (int i = m_columnCount - 1; i >= 0; --i) {
        const TupleSchema::ColumnInfo *columnInfo = m_schema->getColumnInfo(i);
        m_allowNulls[i] = columnInfo->allowNull;
    }

    // Also clear some used block state. this structure doesn't have
    // an block ownership semantics - it's just a cache. I think.
    m_blocksWithSpace.clear();

    // note that any allocated memory in m_data is left alone
    // as is m_allocatedTuples
    m_data.clear();
}

/*
 * Implemented by persistent table and called by Table::loadTuplesFrom
 * to do additional processing for views and Export and non-inline
 * memory tracking
 */
void PersistentTable::processLoadedTuple(TableTuple &tuple,
                                         ReferenceSerializeOutput *uniqueViolationOutput,
                                         int32_t &serializedTupleCount,
                                         size_t &tupleCountPosition,
                                         bool shouldDRStreamRows) {
    try {
        insertTupleCommon(tuple, tuple, true, shouldDRStreamRows);
    } catch (ConstraintFailureException &e) {
        if (uniqueViolationOutput) {
            if (serializedTupleCount == 0) {
                serializeColumnHeaderTo(*uniqueViolationOutput);
                tupleCountPosition = uniqueViolationOutput->reserveBytes(sizeof(int32_t));
            }
            serializedTupleCount++;
            tuple.serializeTo(*uniqueViolationOutput);
            deleteTupleStorage(tuple);
            return;
        } else {
            throw;
        }
    }
}

TableStats* PersistentTable::getTableStats() {
    return &stats_;
}

/** Prepare table for streaming from serialized data. */
bool PersistentTable::activateStream(
    TupleSerializer &tupleSerializer,
    TableStreamType streamType,
    int32_t partitionId,
    CatalogId tableId,
    ReferenceSerializeInputBE &serializeIn) {
    /*
     * Allow multiple stream types for the same partition by holding onto the
     * TableStreamer object. TableStreamer enforces which multiple stream type
     * combinations are allowed. Expect the partition ID not to change.
     */
    assert(m_tableStreamer == NULL || partitionId == m_tableStreamer->getPartitionID());
    if (m_tableStreamer == NULL) {
        m_tableStreamer.reset(new TableStreamer(partitionId, *this, tableId));
    }

    std::vector<std::string> predicateStrings;
    // Grab snapshot or elastic stream predicates.
    if (tableStreamTypeHasPredicates(streamType)) {
        int npreds = serializeIn.readInt();
        if (npreds > 0) {
            predicateStrings.reserve(npreds);
            for (int ipred = 0; ipred < npreds; ipred++) {
                std::string spred = serializeIn.readTextString();
                predicateStrings.push_back(spred);
            }
        }
    }

    return m_tableStreamer->activateStream(m_surgeon, tupleSerializer, streamType, predicateStrings);
}

/**
 * Prepare table for streaming from serialized data (internal for tests).
 * Use custom TableStreamer provided.
 * Return true on success or false if it was already active.
 */
bool PersistentTable::activateWithCustomStreamer(
    TupleSerializer &tupleSerializer,
    TableStreamType streamType,
    boost::shared_ptr<TableStreamerInterface> tableStreamer,
    CatalogId tableId,
    std::vector<std::string> &predicateStrings,
    bool skipInternalActivation) {

    // Expect m_tableStreamer to be null. Only make it fatal in debug builds.
    assert(m_tableStreamer == NULL);
    m_tableStreamer = tableStreamer;
    bool success = !skipInternalActivation;
    if (!skipInternalActivation) {
        success = m_tableStreamer->activateStream(m_surgeon,
                                                  tupleSerializer,
                                                  streamType,
                                                  predicateStrings);
    }
    return success;
}

/**
 * Attempt to serialize more tuples from the table to the provided output streams.
 * Return remaining tuple count, 0 if done, or TABLE_STREAM_SERIALIZATION_ERROR on error.
 */
int64_t PersistentTable::streamMore(TupleOutputStreamProcessor &outputStreams,
                                    TableStreamType streamType,
                                    std::vector<int> &retPositions) {
    if (m_tableStreamer.get() == NULL) {
        char errMsg[1024];
        snprintf(errMsg, 1024, "No table streamer of Type %s for table %s.",
                tableStreamTypeToString(streamType).c_str(), name().c_str());
        LogManager::getThreadLogger(LOGGERID_HOST)->log(LOGLEVEL_ERROR, errMsg);

        return TABLE_STREAM_SERIALIZATION_ERROR;
    }
    return m_tableStreamer->streamMore(outputStreams, streamType, retPositions);
}

/**
 * Process the updates from a recovery message
 */
void PersistentTable::processRecoveryMessage(RecoveryProtoMsg* message, Pool *pool) {
    switch (message->msgType()) {
    case RECOVERY_MSG_TYPE_SCAN_TUPLES: {
        if (isPersistentTableEmpty()) {
            uint32_t tupleCount = message->totalTupleCount();
            BOOST_FOREACH(TableIndex *index, m_indexes) {
                index->ensureCapacity(tupleCount);
            }
        }
        loadTuplesFromNoHeader(*message->stream(), pool);
        break;
    }
    default:
        throwFatalException("Attempted to process a recovery message of unknown type %d", message->msgType());
    }
}

/**
 * Create a tree index on the primary key and then iterate it and hash
 * the tuple data.
 */
size_t PersistentTable::hashCode() {
    boost::scoped_ptr<TableIndex> pkeyIndex(TableIndexFactory::cloneEmptyTreeIndex(*m_pkeyIndex));
    TableIterator iter(this, m_data.begin());
    TableTuple tuple(schema());
    while (iter.next(tuple)) {
        pkeyIndex->addEntry(&tuple, NULL);
    }

    IndexCursor indexCursor(pkeyIndex->getTupleSchema());
    pkeyIndex->moveToEnd(true, indexCursor);

    size_t hashCode = 0;
    while (true) {
         tuple = pkeyIndex->nextValue(indexCursor);
         if (tuple.isNullTuple()) {
             break;
         }
         tuple.hashCode(hashCode);
    }
    return hashCode;
}

void PersistentTable::notifyBlockWasCompactedAway(TBPtr block) {
    if (m_blocksNotPendingSnapshot.find(block) == m_blocksNotPendingSnapshot.end()) {
        // do not find block in not pending snapshot container
        assert(m_tableStreamer.get() != NULL);
        assert(m_blocksPendingSnapshot.find(block) != m_blocksPendingSnapshot.end());
        m_tableStreamer->notifyBlockWasCompactedAway(block);
        return;
    }
    // else check that block is in pending snapshot container
    assert(m_blocksPendingSnapshot.find(block) == m_blocksPendingSnapshot.end());
}

// Call-back from TupleBlock::merge() for each tuple moved.
void PersistentTable::notifyTupleMovement(TBPtr sourceBlock, TBPtr targetBlock,
                                          TableTuple &sourceTuple, TableTuple &targetTuple) {
    if (m_tableStreamer != NULL) {
        m_tableStreamer->notifyTupleMovement(sourceBlock, targetBlock, sourceTuple, targetTuple);
    }
}

void PersistentTable::swapTuples(TableTuple &originalTuple,
                                 TableTuple &destinationTuple) {
    ::memcpy(destinationTuple.address(), originalTuple.address(), m_tupleLength);
    originalTuple.setActiveFalse();
    assert(!originalTuple.isPendingDeleteOnUndoRelease());

    /*
     * If the tuple is pending deletion then it isn't in any of the indexes.
     * However that contradicts the assertion above that the tuple is not
     * pending deletion. In current Volt there is only one transaction executing
     * at any given time and the commit always releases the undo quantum
     * because there is no speculation. This situation should be impossible
     * as the assertion above implies. It looks like this is forward thinking
     * code for something that shouldn't happen right now.
     *
     * However this still isn't sufficient to actually work if speculation
     * is implemented because moving the tuple will invalidate the pointer
     * in the undo action for deleting the tuple. If the transaction ends
     * up being rolled back it won't find the tuple! You would have to go
     * back and update the undo action (how would you find it?) or
     * not move the tuple.
     */
    if (!originalTuple.isPendingDelete()) {
        BOOST_FOREACH(TableIndex *index, m_indexes) {
            if (!index->replaceEntryNoKeyChange(destinationTuple, originalTuple)) {
                throwFatalException("Failed to update tuple in Table: %s Index %s",
                                    m_name.c_str(), index->getName().c_str());
            }
        }
    }
}

bool PersistentTable::doCompactionWithinSubset(TBBucketPtrVector *bucketVector) {
    /**
     * First find the two best candidate blocks
     */
    TBPtr fullest;
    TBBucketI fullestIterator;
    bool foundFullest = false;
    for (int ii = (TUPLE_BLOCK_NUM_BUCKETS - 1); ii >= 0; ii--) {
        fullestIterator = (*bucketVector)[ii]->begin();
        if (fullestIterator != (*bucketVector)[ii]->end()) {
            foundFullest = true;
            fullest = *fullestIterator;
            break;
        }
    }
    if (!foundFullest) {
        //std::cout << "Could not find a fullest block for compaction" << std::endl;
        return false;
    }

    int fullestBucketChange = NO_NEW_BUCKET_INDEX;
    while (fullest->hasFreeTuples()) {
        TBPtr lightest;
        TBBucketI lightestIterator;
        bool foundLightest = false;

        for (int ii = 0; ii < TUPLE_BLOCK_NUM_BUCKETS; ii++) {
            lightestIterator = (*bucketVector)[ii]->begin();
            if (lightestIterator != (*bucketVector)[ii]->end()) {
                lightest = lightestIterator.key();
                if (lightest != fullest) {
                    foundLightest = true;
                    break;
                }
                assert(lightest == fullest);
                lightestIterator++;
                if (lightestIterator != (*bucketVector)[ii]->end()) {
                    lightest = lightestIterator.key();
                    foundLightest = true;
                    break;
                }
            }
        }
        if (!foundLightest) {
            //could not find a lightest block for compaction
            return false;
        }

        std::pair<int, int> bucketChanges = fullest->merge(this, lightest, this);
        int tempFullestBucketChange = bucketChanges.first;
        if (tempFullestBucketChange != NO_NEW_BUCKET_INDEX) {
            fullestBucketChange = tempFullestBucketChange;
        }

        if (lightest->isEmpty()) {
            notifyBlockWasCompactedAway(lightest);
            m_data.erase(lightest->address());
            m_blocksWithSpace.erase(lightest);
            m_blocksNotPendingSnapshot.erase(lightest);
            m_blocksPendingSnapshot.erase(lightest);
            lightest->swapToBucket(TBBucketPtr());
        } else {
            int lightestBucketChange = bucketChanges.second;
            if (lightestBucketChange != NO_NEW_BUCKET_INDEX) {
                lightest->swapToBucket((*bucketVector)[lightestBucketChange]);
            }
        }
    }

    if (fullestBucketChange != NO_NEW_BUCKET_INDEX) {
        fullest->swapToBucket((*bucketVector)[fullestBucketChange]);
    }
    if (!fullest->hasFreeTuples()) {
        m_blocksWithSpace.erase(fullest);
    }
    return true;
}

void PersistentTable::doIdleCompaction() {
    if (!m_blocksNotPendingSnapshot.empty()) {
        doCompactionWithinSubset(&m_blocksNotPendingSnapshotLoad);
    }
    if (!m_blocksPendingSnapshot.empty()) {
        doCompactionWithinSubset(&m_blocksPendingSnapshotLoad);
    }
}

bool PersistentTable::doForcedCompaction() {
    if (m_tableStreamer.get() != NULL && m_tableStreamer->hasStreamType(TABLE_STREAM_RECOVERY)) {
        LogManager::getThreadLogger(LOGGERID_SQL)->log(LOGLEVEL_INFO,
            "Deferring compaction until recovery is complete.");
        return false;
    }
    bool hadWork1 = true;
    bool hadWork2 = true;
    int64_t notPendingCompactions = 0;
    int64_t pendingCompactions = 0;

    char msg[512];
    snprintf(msg, sizeof(msg), "Doing forced compaction with allocated tuple count %zd",
             ((intmax_t)allocatedTupleCount()));
    LogManager::getThreadLogger(LOGGERID_SQL)->log(LOGLEVEL_INFO, msg);

    int failedCompactionCountBefore = m_failedCompactionCount;
    while (compactionPredicate()) {
        assert(hadWork1 || hadWork2);
        if (!hadWork1 && !hadWork2) {
            /*
             * If this code is reached it means that the compaction predicate
             * thinks that it should be possible to merge some blocks,
             * but there were no blocks found in the load buckets that were
             * eligible to be merged. This is a bug in either the predicate
             * or more likely the code that moves blocks from bucket to bucket.
             * This isn't fatal because the list of blocks with free space
             * and deletion of empty blocks is handled independently of
             * the book keeping for load buckets and merging. As the load
             * of the missing (missing from the load buckets)
             * blocks changes they should end up being inserted
             * into the bucketing system again and will be
             * compacted if necessary or deleted when empty.
             * This is a work around for ENG-939
             */
            if (m_failedCompactionCount % 5000 == 0) {
                snprintf(msg, sizeof(msg), "Compaction predicate said there should be "
                         "blocks to compact but no blocks were found "
                         "to be eligible for compaction. This has "
                         "occured %d times.", m_failedCompactionCount);
                LogManager::getThreadLogger(LOGGERID_SQL)->log(LOGLEVEL_ERROR, msg);
            }
            if (m_failedCompactionCount == 0) {
                printBucketInfo();
            }
            m_failedCompactionCount++;
            break;
        }
        if (!m_blocksNotPendingSnapshot.empty() && hadWork1) {
            //std::cout << "Compacting blocks not pending snapshot " << m_blocksNotPendingSnapshot.size() << std::endl;
            hadWork1 = doCompactionWithinSubset(&m_blocksNotPendingSnapshotLoad);
            notPendingCompactions++;
        }
        if (!m_blocksPendingSnapshot.empty() && hadWork2) {
            //std::cout << "Compacting blocks pending snapshot " << m_blocksPendingSnapshot.size() << std::endl;
            hadWork2 = doCompactionWithinSubset(&m_blocksPendingSnapshotLoad);
            pendingCompactions++;
        }
    }
    //If compactions have been failing lately, but it didn't fail this time
    //then compaction progressed until the predicate was satisfied
    if (failedCompactionCountBefore > 0 && failedCompactionCountBefore == m_failedCompactionCount) {
        snprintf(msg, sizeof(msg), "Recovered from a failed compaction scenario "
                "and compacted to the point that the compaction predicate was "
                "satisfied after %d failed attempts", failedCompactionCountBefore);
        LogManager::getThreadLogger(LOGGERID_SQL)->log(LOGLEVEL_ERROR, msg);
        m_failedCompactionCount = 0;
    }

    assert(!compactionPredicate());
    snprintf(msg, sizeof(msg), "Finished forced compaction of %zd non-snapshot blocks and %zd snapshot blocks with allocated tuple count %zd",
            ((intmax_t)notPendingCompactions), ((intmax_t)pendingCompactions), ((intmax_t)allocatedTupleCount()));
    LogManager::getThreadLogger(LOGGERID_SQL)->log(LOGLEVEL_INFO, msg);
    return (notPendingCompactions + pendingCompactions) > 0;
}

void PersistentTable::printBucketInfo() {
    std::cout << std::endl;
    TBMapI iter = m_data.begin();
    while (iter != m_data.end()) {
        std::cout << "Block " << static_cast<void*>(iter.data()->address()) << " has " <<
                iter.data()->activeTuples() << " active tuples and " << iter.data()->lastCompactionOffset()
                << " last compaction offset and is in bucket " <<
                static_cast<void*>(iter.data()->currentBucket().get()) <<
                std::endl;
        iter++;
    }

    boost::unordered_set<TBPtr>::iterator blocksNotPendingSnapshot = m_blocksNotPendingSnapshot.begin();
    std::cout << "Blocks not pending snapshot: ";
    while (blocksNotPendingSnapshot != m_blocksNotPendingSnapshot.end()) {
        std::cout << static_cast<void*>((*blocksNotPendingSnapshot)->address()) << ",";
        blocksNotPendingSnapshot++;
    }
    std::cout << std::endl;
    for (int ii = 0; ii < m_blocksNotPendingSnapshotLoad.size(); ii++) {
        if (m_blocksNotPendingSnapshotLoad[ii]->empty()) {
            continue;
        }
        std::cout << "Bucket " << ii << "(" << static_cast<void*>(m_blocksNotPendingSnapshotLoad[ii].get()) << ") has size " << m_blocksNotPendingSnapshotLoad[ii]->size() << std::endl;
        TBBucketI bucketIter = m_blocksNotPendingSnapshotLoad[ii]->begin();
        while (bucketIter != m_blocksNotPendingSnapshotLoad[ii]->end()) {
            std::cout << "\t" << static_cast<void*>((*bucketIter)->address()) << std::endl;
            bucketIter++;
        }
    }

    boost::unordered_set<TBPtr>::iterator blocksPendingSnapshot = m_blocksPendingSnapshot.begin();
    std::cout << "Blocks pending snapshot: ";
    while (blocksPendingSnapshot != m_blocksPendingSnapshot.end()) {
        std::cout << static_cast<void*>((*blocksPendingSnapshot)->address()) << ",";
        blocksPendingSnapshot++;
    }
    std::cout << std::endl;
    for (int ii = 0; ii < m_blocksPendingSnapshotLoad.size(); ii++) {
        if (m_blocksPendingSnapshotLoad[ii]->empty()) {
            continue;
        }
        std::cout << "Bucket " << ii << "(" << static_cast<void*>(m_blocksPendingSnapshotLoad[ii].get()) << ") has size " << m_blocksPendingSnapshotLoad[ii]->size() << std::endl;
        TBBucketI bucketIter = m_blocksPendingSnapshotLoad[ii]->begin();
        while (bucketIter != m_blocksPendingSnapshotLoad[ii]->end()) {
            std::cout << "\t" << static_cast<void*>((*bucketIter)->address()) << std::endl;
            bucketIter++;
        }
    }
    std::cout << std::endl;
}

int64_t PersistentTable::validatePartitioning(TheHashinator *hashinator, int32_t partitionId) {
    TableIterator iter = iterator();

    int64_t mispartitionedRows = 0;

    while (iter.hasNext()) {
        TableTuple tuple(schema());
        iter.next(tuple);
        if (hashinator->hashinate(tuple.getNValue(m_partitionColumn)) != partitionId) {
            mispartitionedRows++;
        }
    }
    return mispartitionedRows;
}

void PersistentTableSurgeon::activateSnapshot() {
    //All blocks are now pending snapshot
    m_table.m_blocksPendingSnapshot.swap(m_table.m_blocksNotPendingSnapshot);
    m_table.m_blocksPendingSnapshotLoad.swap(m_table.m_blocksNotPendingSnapshotLoad);
    assert(m_table.m_blocksNotPendingSnapshot.empty());
    for (int ii = 0; ii < m_table.m_blocksNotPendingSnapshotLoad.size(); ii++) {
        assert(m_table.m_blocksNotPendingSnapshotLoad[ii]->empty());
    }
}

std::pair<const TableIndex*, uint32_t> PersistentTable::getUniqueIndexForDR() {
    // In active-active we always send full tuple instead of just index tuple.
    bool isActiveActive = ExecutorContext::getExecutorContext()->getEngine()->getIsActiveActiveDREnabled();
    if (isActiveActive) {
        TableIndex* nullIndex = NULL;
        return std::make_pair(nullIndex, 0);
    }

    if (!m_smallestUniqueIndex && !m_noAvailableUniqueIndex) {
        computeSmallestUniqueIndex();
    }
    return std::make_pair(m_smallestUniqueIndex, m_smallestUniqueIndexCrc);
}

void PersistentTable::computeSmallestUniqueIndex() {
    uint32_t smallestIndexTupleLength = UINT32_MAX;
    m_noAvailableUniqueIndex = true;
    m_smallestUniqueIndex = NULL;
    m_smallestUniqueIndexCrc = 0;
    std::string smallestUniqueIndexName = ""; // use name for determinism
    BOOST_FOREACH(TableIndex* index, m_indexes) {
        if (index->isUniqueIndex() && !index->isPartialIndex()) {
            uint32_t indexTupleLength = index->getKeySchema()->tupleLength();
            if (!m_smallestUniqueIndex ||
                (m_smallestUniqueIndex->keyUsesNonInlinedMemory() && !index->keyUsesNonInlinedMemory()) ||
                indexTupleLength < smallestIndexTupleLength ||
                (indexTupleLength == smallestIndexTupleLength && index->getName() < smallestUniqueIndexName)) {
                m_smallestUniqueIndex = index;
                m_noAvailableUniqueIndex = false;
                smallestIndexTupleLength = indexTupleLength;
                smallestUniqueIndexName = index->getName();
            }
        }
    }
    if (m_smallestUniqueIndex) {
        m_smallestUniqueIndexCrc = vdbcrc::crc32cInit();
        m_smallestUniqueIndexCrc = vdbcrc::crc32c(m_smallestUniqueIndexCrc,
                &(m_smallestUniqueIndex->getColumnIndices()[0]),
                m_smallestUniqueIndex->getColumnIndices().size() * sizeof(int));
        m_smallestUniqueIndexCrc = vdbcrc::crc32cFinish(m_smallestUniqueIndexCrc);
    }
}

} // namespace voltdb
