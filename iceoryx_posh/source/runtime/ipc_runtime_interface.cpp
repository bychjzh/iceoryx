// Copyright (c) 2019 - 2021 by Robert Bosch GmbH. All rights reserved.
// Copyright (c) 2020 - 2022 by Apex.AI Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "iceoryx_posh/internal/runtime/ipc_runtime_interface.hpp"
#include "iceoryx_posh/internal/posh_error_reporting.hpp"
#include "iceoryx_posh/version/version_info.hpp"
#include "iox/assertions.hpp"
#include "iox/detail/convert.hpp"
#include "iox/into.hpp"
#include "iox/posix_user.hpp"
#include "iox/std_string_support.hpp"

#include <thread>

namespace iox
{
namespace runtime
{
IpcRuntimeInterface::IpcRuntimeInterface(const RuntimeName_t& roudiName,
                                         const RuntimeName_t& runtimeName,
                                         const units::Duration roudiWaitingTimeout) noexcept
    : m_runtimeName(runtimeName)
    , m_RoudiIpcInterface(roudiName)
{
    m_AppIpcInterface.emplace(runtimeName);
    if (!m_AppIpcInterface->isInitialized())
    {
        IOX_REPORT_FATAL(PoshError::IPC_INTERFACE__UNABLE_TO_CREATE_APPLICATION_CHANNEL);
        return;
    }

    deadline_timer timer(roudiWaitingTimeout);

    enum class RegState
    {
        WAIT_FOR_ROUDI,
        SEND_REGISTER_REQUEST,
        WAIT_FOR_REGISTER_ACK,
        FINISHED
    };

    int64_t transmissionTimestamp{0};
    auto regState = RegState::WAIT_FOR_ROUDI;
    while (!timer.hasExpired() && regState != RegState::FINISHED)
    {
        if (!m_RoudiIpcInterface.isInitialized() || !m_RoudiIpcInterface.ipcChannelMapsToFile())
        {
            IOX_LOG(DEBUG, "reopen RouDi's IPC channel!");
            m_RoudiIpcInterface.reopen();
            regState = RegState::WAIT_FOR_ROUDI;
        }

        switch (regState)
        {
        case RegState::WAIT_FOR_ROUDI:
        {
            waitForRoudi(timer);
            if (m_RoudiIpcInterface.isInitialized())
            {
                regState = RegState::SEND_REGISTER_REQUEST;
            }
            break;
        }
        case RegState::SEND_REGISTER_REQUEST:
        {
            using namespace units;
            using namespace std::chrono;
            auto timestamp = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
            while (transmissionTimestamp == timestamp)
            {
                timestamp = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
            }
            transmissionTimestamp = timestamp;

            // send IpcMessageType::REG to RouDi

            IpcMessage sendBuffer;
            int pid = getpid();
            IOX_ENFORCE(pid >= 0, "'getpid' must always return a positive number");
            sendBuffer << IpcMessageTypeToString(IpcMessageType::REG) << m_runtimeName << convert::toString(pid)
                       << convert::toString(PosixUser::getUserOfCurrentProcess().getID())
                       << convert::toString(transmissionTimestamp)
                       << static_cast<Serialization>(version::VersionInfo::getCurrentVersion()).toString();

            bool successfullySent = m_RoudiIpcInterface.timedSend(sendBuffer, 100_ms);

            if (successfullySent)
            {
                regState = RegState::WAIT_FOR_REGISTER_ACK;
            }
            else
            {
                regState = RegState::WAIT_FOR_ROUDI;
            }
            break;
        }
        case RegState::WAIT_FOR_REGISTER_ACK:
            if (waitForRegAck(transmissionTimestamp) == RegAckResult::SUCCESS)
            {
                regState = RegState::FINISHED;
            }
            else
            {
                regState = RegState::WAIT_FOR_ROUDI;
            }
            break;
        case RegState::FINISHED:
            // nothing to do, move along
            break;
        }
    }

    if (regState != RegState::FINISHED)
    {
        m_AppIpcInterface.reset();
    }
    switch (regState)
    {
    case RegState::WAIT_FOR_ROUDI:
        IOX_LOG(FATAL, "Timeout registering at RouDi. Is RouDi running?");
        IOX_REPORT_FATAL(PoshError::IPC_INTERFACE__REG_ROUDI_NOT_AVAILABLE);
        break;
    case RegState::SEND_REGISTER_REQUEST:
        IOX_REPORT_FATAL(PoshError::IPC_INTERFACE__REG_UNABLE_TO_WRITE_TO_ROUDI_CHANNEL);
        break;
    case RegState::WAIT_FOR_REGISTER_ACK:
        IOX_REPORT_FATAL(PoshError::IPC_INTERFACE__REG_ACK_NO_RESPONSE);
        break;
    case RegState::FINISHED:
        // nothing to do, move along
        break;
    }
}

UntypedRelativePointer::offset_t IpcRuntimeInterface::getSegmentManagerAddressOffset() const noexcept
{
    IOX_ENFORCE(m_segmentManagerAddressOffset.has_value(),
                "No segment manager available! Should have been fetched in the c'tor");
    return m_segmentManagerAddressOffset.value();
}

bool IpcRuntimeInterface::sendRequestToRouDi(const IpcMessage& msg, IpcMessage& answer) noexcept
{
    if (!m_RoudiIpcInterface.send(msg))
    {
        IOX_LOG(ERROR, "Could not send request via RouDi IPC channel interface.\n");
        return false;
    }

    if (!m_AppIpcInterface->receive(answer))
    {
        IOX_LOG(ERROR, "Could not receive request via App IPC channel interface.\n");
        return false;
    }

    return true;
}

size_t IpcRuntimeInterface::getShmTopicSize() noexcept
{
    return m_shmTopicSize;
}

void IpcRuntimeInterface::waitForRoudi(deadline_timer& timer) noexcept
{
    bool printWaitingWarning = true;
    bool printFoundMessage = false;
    uint32_t numberOfRemainingFastPolls{10};
    while (!timer.hasExpired() && !m_RoudiIpcInterface.isInitialized())
    {
        m_RoudiIpcInterface.reopen();

        if (m_RoudiIpcInterface.isInitialized())
        {
            IOX_LOG(DEBUG, "RouDi IPC Channel found!");
            break;
        }

        if (printWaitingWarning)
        {
            IOX_LOG(WARN, "RouDi not found - waiting ...");
            printWaitingWarning = false;
            printFoundMessage = true;
        }
        if (numberOfRemainingFastPolls > 0)
        {
            --numberOfRemainingFastPolls;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (printFoundMessage && m_RoudiIpcInterface.isInitialized())
    {
        IOX_LOG(WARN, "... RouDi found.");
    }
}

IpcRuntimeInterface::RegAckResult IpcRuntimeInterface::waitForRegAck(int64_t transmissionTimestamp) noexcept
{
    // wait for the register ack from the RouDi daemon. If we receive another response we do a retry
    constexpr size_t MAX_RETRY_COUNT = 3;
    size_t retryCounter = 0;
    while (retryCounter++ < MAX_RETRY_COUNT)
    {
        using namespace units::duration_literals;
        IpcMessage receiveBuffer;
        // wait for IpcMessageType::REG_ACK from RouDi for 1 seconds
        if (m_AppIpcInterface->timedReceive(1_s, receiveBuffer))
        {
            std::string cmd = receiveBuffer.getElementAtIndex(0U);

            if (stringToIpcMessageType(cmd.c_str()) == IpcMessageType::REG_ACK)
            {
                constexpr uint32_t REGISTER_ACK_PARAMETERS = 6U;
                if (receiveBuffer.getNumberOfElements() != REGISTER_ACK_PARAMETERS)
                {
                    IOX_REPORT_FATAL(PoshError::IPC_INTERFACE__REG_ACK_INVALIG_NUMBER_OF_PARAMS);
                }

                // read out the shared memory base address and save it
                UntypedRelativePointer::offset_t segmentManagerOffset{UntypedRelativePointer::NULL_POINTER_OFFSET};
                UntypedRelativePointer::offset_t heartbeatOffset{UntypedRelativePointer::NULL_POINTER_OFFSET};
                int64_t receivedTimestamp{0U};

                auto topic_size_result =
                    iox::convert::from_string<uint64_t>(receiveBuffer.getElementAtIndex(1U).c_str());
                auto segment_manager_offset_result =
                    iox::convert::from_string<uintptr_t>(receiveBuffer.getElementAtIndex(2U).c_str());
                auto recv_timestamp_result =
                    iox::convert::from_string<int64_t>(receiveBuffer.getElementAtIndex(3U).c_str());
                auto segment_id_result =
                    iox::convert::from_string<uint64_t>(receiveBuffer.getElementAtIndex(4U).c_str());
                auto heartbeat_offset_result =
                    iox::convert::from_string<uintptr_t>(receiveBuffer.getElementAtIndex(5U).c_str());

                // validate conversion results
                if (!topic_size_result.has_value() || !segment_manager_offset_result.has_value()
                    || !recv_timestamp_result.has_value() || !segment_id_result.has_value()
                    || !heartbeat_offset_result.has_value())
                {
                    return RegAckResult::MALFORMED_RESPONSE;
                }

                // assign conversion results
                m_shmTopicSize = topic_size_result.value();
                m_segmentId = segment_id_result.value();
                segmentManagerOffset = segment_manager_offset_result.value();
                receivedTimestamp = recv_timestamp_result.value();
                heartbeatOffset = heartbeat_offset_result.value();

                m_segmentManagerAddressOffset.emplace(segmentManagerOffset);

                if (heartbeatOffset != UntypedRelativePointer::NULL_POINTER_OFFSET)
                {
                    m_heartbeatAddressOffset = heartbeatOffset;
                }

                if (transmissionTimestamp == receivedTimestamp)
                {
                    return RegAckResult::SUCCESS;
                }
                else
                {
                    IOX_LOG(WARN, "Received a REG_ACK with an outdated timestamp!");
                }
            }
            else
            {
                IOX_LOG(ERROR, "Wrong response received " << receiveBuffer.getMessage());
            }
        }
    }

    return RegAckResult::TIMEOUT;
}

uint64_t IpcRuntimeInterface::getSegmentId() const noexcept
{
    return m_segmentId;
}

optional<UntypedRelativePointer::offset_t> IpcRuntimeInterface::getHeartbeatAddressOffset() const noexcept
{
    return m_heartbeatAddressOffset;
}

} // namespace runtime
} // namespace iox
