/* This file is part of VoltDB.
 * Copyright (C) 2008-2012 VoltDB Inc.
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "common/SQLException.h"
#include "common/SerializableEEException.h"
#include "common/serializeio.h"
#include <iostream>
#include <cassert>

using namespace voltdb;

// Please keep these ordered alphabetically.
// Names and codes are standardized.
const char* SQLException::data_exception_division_by_zero = "22012";
const char* SQLException::data_exception_invalid_parameter = "22023";
const char* SQLException::data_exception_most_specific_type_mismatch = "2200G";
const char* SQLException::data_exception_numeric_value_out_of_range = "22003";
const char* SQLException::data_exception_string_data_length_mismatch = "22026";
const char* SQLException::dynamic_sql_error = "07000";
const char* SQLException::integrity_constraint_violation = "23000";

// This is non-standard -- keep it unique.
const char* SQLException::nonspecific_error_code_for_error_forced_by_user = "99999";
const char* SQLException::specific_error_specified_by_user = "Specific error code specified by user invocation of SQL_ERROR";


// These are ordered by error code. Names and codes are volt
// specific - must find merge conflicts on duplicate codes.
const char* SQLException::volt_output_buffer_overflow = "V0001";
const char* SQLException::volt_temp_table_memory_overflow = "V0002";
const char* SQLException::volt_decimal_serialization_error = "V0003";

SQLException::SQLException(const char* sqlState, std::string message) :
    SerializableEEException(VOLT_EE_EXCEPTION_TYPE_SQL, message),
    m_sqlState(sqlState), m_internalFlags(0)
{
    assert(m_sqlState);
    assert(m_sqlState[5] == '\0');
    assert(strlen(m_sqlState) == 5);
}

SQLException::SQLException(const char* sqlState, std::string message, VoltEEExceptionType type) :
    SerializableEEException(type, message),
    m_sqlState(sqlState), m_internalFlags(0)
{
    assert(m_sqlState);
    assert(m_sqlState[5] == '\0');
    assert(strlen(m_sqlState) == 5);
}

SQLException::SQLException(const char* sqlState, std::string message, int internalFlags) :
    SerializableEEException(VOLT_EE_EXCEPTION_TYPE_SQL, message),
    m_sqlState(sqlState), m_internalFlags(internalFlags)
{
    assert(m_sqlState);
    assert(m_sqlState[5] == '\0');
    assert(strlen(m_sqlState) == 5);
}

void SQLException::p_serialize(ReferenceSerializeOutput *output) {
    // Asserting these conditions again here "at the last second", just in case the buffer was initialized to something
    // dynamic that might have gone out of scope in the process of throwing and catching this exception.
    // Usually, the state is initialized to one of the const char* statics above,
    // so there's no need to "copy in" in the constructor or to worry about it having changed here,
    // but user-defined functions throwing user-defined SQL errors present a special challenge,
    // especially for some values of "user".
    assert(m_sqlState);
    assert(m_sqlState[5] == '\0');
    assert(strlen(m_sqlState) == 5);
    for (int ii = 0; ii < 5; ii++) {
        output->writeByte(m_sqlState[ii]);
    }
}
