/* This file is part of VoltDB.
 * Copyright (C) 2008-2016 VoltDB Inc.
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

#ifndef DRTUPLESTREAM_H_
#define DRTUPLESTREAM_H_

#include "storage/AbstractDRTupleStream.h"

namespace voltdb {
class StreamBlock;
class TableIndex;

class DRTupleStream : public voltdb::AbstractDRTupleStream {
public:
    //Version(1), type(1), drId(8), uniqueId(8), hashFlag(1), txnLength(4), parHash(4)
    static const size_t BEGIN_RECORD_SIZE = 1 + 1 + 8 + 8 + 1 + 4 + 4;
    //Version(1), type(1), drId(8), uniqueId(8)
    static const size_t BEGIN_RECORD_HEADER_SIZE = 1 + 1 + 8 + 8;
    //Type(1), drId(8), checksum(4)
    static const size_t END_RECORD_SIZE = 1 + 8 + 4;
    //Type(1), table signature(8)
    static const size_t TXN_RECORD_HEADER_SIZE = 1 + 8;
    //Type(1), parHash(4)
    static const size_t HASH_DELIMITER_SIZE = 1 + 4;

    // Also update DRProducerProtocol.java if version changes
    static const uint8_t PROTOCOL_VERSION = 4;

    DRTupleStream();

    virtual ~DRTupleStream() {
    }

    void configure(CatalogId partitionId) {
        AbstractDRTupleStream::configure(partitionId);
        m_hashFlag = (partitionId == 16383) ? 1 : 0;
        m_firstParHash = 0;
    }

    /**
     * write an insert or delete record to the stream
     * for active-active conflict detection purpose, write full row image for delete records.
     * */
    virtual size_t appendTuple(int64_t lastCommittedSpHandle,
                       char *tableHandle,
                       int partitionColumn,
                       int64_t txnId,
                       int64_t spHandle,
                       int64_t uniqueId,
                       TableTuple &tuple,
                       DRRecordType type,
                       const std::pair<const TableIndex*, uint32_t>& indexPair);

    /**
     * write an update record to the stream
     * for active-active conflict detection purpose, write full before image for update records.
     * */
    virtual size_t appendUpdateRecord(int64_t lastCommittedSpHandle,
                       char *tableHandle,
                       int partitionColumn,
                       int64_t txnId,
                       int64_t spHandle,
                       int64_t uniqueId,
                       TableTuple &oldTuple,
                       TableTuple &newTuple,
                       const std::pair<const TableIndex*, uint32_t>& indexPair);

    virtual size_t truncateTable(int64_t lastCommittedSpHandle,
                       char *tableHandle,
                       std::string tableName,
                       int64_t txnId,
                       int64_t spHandle,
                       int64_t uniqueId);

    virtual void beginTransaction(int64_t sequenceNumber, int64_t uniqueId);
    // If a transaction didn't generate any binary log data, calling this
    // would be a no-op because it was never begun.
    virtual void endTransaction(int64_t uniqueId);

    virtual bool checkOpenTransaction(StreamBlock *sb, size_t minLength, size_t& blockSize, size_t& uso);

    virtual DRCommittedInfo getLastCommittedSequenceNumberAndUniqueIds() {
        return DRCommittedInfo(m_committedSequenceNumber, m_lastCommittedSpUniqueId, m_lastCommittedMpUniqueId);
    }

    static int32_t getTestDRBuffer(int32_t partitionKeyValue, int32_t partitionId, int32_t flag, char *out);
protected:
    int8_t m_hashFlag; // 1 replicated 2 single 4 multi 8 special
    int64_t m_firstParHash;
    int64_t m_lastParHash;
    size_t m_beginTxnUso;
private:
    void transactionChecks(int64_t lastCommittedSpHandle, int64_t txnId, int64_t spHandle, int64_t uniqueId);

    void writeRowTuple(TableTuple& tuple,
            size_t rowHeaderSz,
            size_t rowMetadataSz,
            const std::vector<int> *interestingColumns,
            const std::pair<const TableIndex*, uint32_t> &indexPair,
            ExportSerializeOutput &io);

    size_t computeOffsets(DRRecordType &type,
            const std::pair<const TableIndex*, uint32_t> &indexPair,
            TableTuple &tuple,
            size_t &rowHeaderSz,
            size_t &rowMetadataSz,
            const std::vector<int> *&interestingColumns);

    int64_t getParHashForTuple(TableTuple& tuple, int partitionColumn);
    bool updateParHash(int64_t parHash);

    int64_t m_lastCommittedSpUniqueId;
    int64_t m_lastCommittedMpUniqueId;
};

class MockDRTupleStream : public DRTupleStream {
public:
    MockDRTupleStream() : DRTupleStream() {}
    size_t appendTuple(int64_t lastCommittedSpHandle,
                           char *tableHandle,
                           int partitionColumn,
                           int64_t txnId,
                           int64_t spHandle,
                           int64_t uniqueId,
                           TableTuple &tuple,
                           DRRecordType type,
                           const std::pair<const TableIndex*, uint32_t>& indexPair) {
        return 0;
    }

    void pushExportBuffer(StreamBlock *block, bool sync, bool endOfStream) {}

    void rollbackTo(size_t mark, size_t drRowCost) {}

    size_t truncateTable(int64_t lastCommittedSpHandle,
                       char *tableHandle,
                       std::string tableName,
                       int64_t txnId,
                       int64_t spHandle,
                       int64_t uniqueId) {
        return 0;
    }
};

}

#endif
