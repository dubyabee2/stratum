/*
 * Copyright 2018 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef STRATUM_HAL_LIB_COMMON_SWITCH_INTERFACE_H_
#define STRATUM_HAL_LIB_COMMON_SWITCH_INTERFACE_H_

#include <vector>
#include <memory>
#include <string>

#include "stratum/glue/status/status.h"
#include "stratum/glue/status/statusor.h"
#include "stratum/hal/lib/common/common.pb.h"
#include "stratum/hal/lib/common/gnmi_events.h"
#include "stratum/hal/lib/common/writer_interface.h"
#include "stratum/lib/channel/channel.h"
#include "p4/v1/p4runtime.grpc.pb.h"

namespace stratum {
namespace hal {

// The "SwitchInterface" class in HAL encapsulates all the main functionalities
// required to initialized, control and monitor a switch ASIC. This is an
// abstract class, allowing several implementation of a low-level switch ASIC
// interface, without changing the higher-level HAL interface to the controller.
class SwitchInterface {
 public:
  virtual ~SwitchInterface() {}

  // Configures the switch based on the given ChassisConfig proto. ChassisConfig
  // proto encapsulates the configuration data for all different parts of the
  // stack (chassis, nodes, ports, etc.). However, this proto does not include
  // P4-based forwarding pipeline config which is generated by a P4 compiler.
  // P4-based forwarding pipeline config is pushed (most of the time after
  // PushChassisConfig() is called at least once after switch comes up) by
  // PushForwardingPipelineConfig(). Note that this function is expected to call
  // VerifyChassisConfig() to explicitly verify the config before pushing
  // anything to the hardware. It is also expected to perform the coldboot init
  // sequence if the switch is not yet initialized by the time config is pushed
  // in the coldboot mode. In other word, the caller does not need to
  // explicitly initialize the class in the coldboot mode, and that is why
  // there is no public Initialize() method defined in this class. In the
  // warmboot mode, however, this function is called after Unfreeze(), which
  // performs the warmboot init sequence. Therefore, PushChassisConfig() will
  // not perform any warmboot initialization:
  // Coldboot start: 1st PushChassisConfig() -> coldboot init seq ->
  //                 next PushChassisConfig()
  // Warmboot start: Unfreeze() -> warmboot init -> PushChassisConfig().
  // It should be noted that when this method is called, the given ChassisConfig
  // proto may or may not have all the messages populated. The method is
  // expected to handle partially populated ChassisConfig protos seamlessly.
  virtual ::util::Status PushChassisConfig(const ChassisConfig& config) = 0;

  // Verifies the given ChassisConfig proto without pushing anything to the
  // hardware. Note that PushChassisConfig() calls VerifyChassisConfig() at
  // the beginning before performing the push. Also, VerifyChassisConfig() must
  // be callable at any point before/after switch is initialized in the
  // coldboot/warmboot mode.
  virtual ::util::Status VerifyChassisConfig(const ChassisConfig& config) = 0;

  // Pushes the P4-based forwarding pipeline configuration of a switching node.
  // ::p4::ForwardingPipelineConfig proto passed to this method is generated by
  // a P4 compiler and is conceptually different from ChassisConfig passed to
  // PushChassisConfig(). It includes parameters that specify the logical
  // forwarding pipeline (tables, action profiles, etc.) for one switching node.
  // This method shall be called after the first PushChassisConfig() which
  // initializes the switch. The forwarding pipeline config needs to be pushed
  // before any flow/group programming can be done. Calling this function after
  // the switch is initialized may require reboot.
  virtual ::util::Status PushForwardingPipelineConfig(
      uint64 node_id, const ::p4::v1::ForwardingPipelineConfig& config) = 0;

  // Saves a new P4-based forwarding pipeline configuration for the switching
  // node but does not commit it to HW (see P4Runtime
  // ::p4::SetForwardingPipelineConfigRequest::VERIFY_AND_SAVE action). After
  // this call completes, the underlying switch should keep processing packets
  // with the previous forwarding pipeline configuration but should start
  // accepting flows for the new forwarding pipeline configuration. This call
  // must be followed by a call to CommitForwardingPipelineConfig.
  virtual ::util::Status SaveForwardingPipelineConfig(
      uint64 node_id, const ::p4::v1::ForwardingPipelineConfig& config) = 0;

  // Commits the forwarding pipeline configuration which was previously saved
  // with SaveForwardingPipelineConfig. See the P4Runtime
  // ::p4::SetForwardingPipelineConfigRequest::COMMIT action. After this call
  // completes, the underlying switch should be processing packets according to
  // the new forwarding pipeline configuration.
  virtual ::util::Status CommitForwardingPipelineConfig(uint64 node_id) = 0;

  // Verifies the P4-based forwarding pipeline specifications of a switching
  // node without programming anything to the hardware. It is expected that
  // PushForwardingPipelineConfig() calls VerifyForwardingPipelineConfig() to
  // verify the forwarding config before pushing anything to the hardware. Also,
  // this funcation can be called at any point before/after the switch is
  // initialize or the chassis config is pushed.
  virtual ::util::Status VerifyForwardingPipelineConfig(
      uint64 node_id, const ::p4::v1::ForwardingPipelineConfig& config) = 0;

  // Performs the shutdown sequence in coldboot mode. This function is not
  // called to prepare for shutdown in warmboot mode. The warmboot shutdown
  // sequence is performed in Freeze(). However, it is required that calling
  // Shutdown() after Freeze() is safe (i.e. NOOP) and does not disrupt the
  // traffic and/or change the hardware state.
  // Coldboot shutdown: Shutdown() -> coldboot shutdown seq -> exit
  // Warmboot shutdown: Freeze() -> warmboot shutdown seq -> exit
  virtual ::util::Status Shutdown() = 0;

  // Performs NSF freeze. It performs the warmboot shutdown sequence and saves
  // the checkpoint to local storage. All the hardware-related warmboot shutdown
  // steps are done in Freeze(), and calling Shutdown() after Freeze() is safe
  // and does not disrupt the traffic and/or change the hardware state (NOOP).
  // Also, calling Freeze() and then Unfreeze() any number of times must be
  // non-disruptive to forwarding pipeline and must bring the switch back to the
  // state before calling Freeze().
  virtual ::util::Status Freeze() = 0;

  // Performs NSF unfreeze. Restores checkpoint and initialized the switch in
  // the warmboot mode. This method needs to be called before any
  // PushChassisConfig() in the warmboot mode. Also, calling Freeze() and then
  // Unfreeze() any number of times must be non-disruptive to forwarding
  // pipeline and bring the switch back to the state before calling Freeze().
  virtual ::util::Status Unfreeze() = 0;

  // Writes P4-based forwarding entries (table entries, action profile members,
  // action profile groups, meters, counters) to a specific switching node.
  // This method can be called only after a successful call to
  // PushForwardingPipelineConfig(). The method populates the vector `results`
  // with the results of writing individual forwarding entries. If `results`
  // is non-empty after this method returns, the size of `results` will be the
  // same as entries in `req`, with element #i in result holding the result of
  // writing forwarding entry #i in `req`.
  virtual ::util::Status WriteForwardingEntries(
      const ::p4::v1::WriteRequest& req,
      std::vector<::util::Status>* results) = 0;

  // Reads P4-based forwarding entries (table entries, action profile members,
  // action profile groups, meters, counters) from a specific switching node.
  // This method can be called only after a successful call to
  // PushForwardingPipelineConfig(). The given ::p4::ReadRequest will include
  // the id of the switching node as well as the forwarding entites to read.
  // If `details` is not nullptr, this method "may" populate the vector with
  // extra details about the results of reading the entries requested (e.g. if
  // some of the entries requested are not supported, the returned status may
  // still be OK and the non supported entries will be specified in `details`).
  // Note that there is no requirement on the order of entries in this vector.
  virtual ::util::Status ReadForwardingEntries(
      const ::p4::v1::ReadRequest& req,
      WriterInterface<::p4::v1::ReadResponse>* writer,
      std::vector<::util::Status>* details) = 0;

  // Registers a writer to be invoked when we receive a packet on any port on
  // the specified node which are destined for the controller. The sent
  // ::p4::PacketIn instance includes all the info on where the packet was
  // received on this node as well as its payload.
  virtual ::util::Status RegisterPacketReceiveWriter(
      uint64 node_id,
      std::shared_ptr<WriterInterface<::p4::v1::PacketIn>> writer) = 0;

  // Unregisters the writer registered to this node by
  // RegisterPacketReceiveWriter().
  virtual ::util::Status UnregisterPacketReceiveWriter(uint64 node_id) = 0;

  // Transmits a packet received from controller directly to a port on a given
  // node (specified by 'node_id') or to the ingress pipeline of the node
  // to let the chip route the packet. The given ::p4::PacketOut instance
  // includes all the info on where to transmit the packet as well as its
  // payload.
  virtual ::util::Status TransmitPacket(uint64 node_id,
                                        const ::p4::v1::PacketOut& packet) = 0;

  // Registers a writer for sending gNMI events.
  virtual ::util::Status RegisterEventNotifyWriter(
      std::shared_ptr<WriterInterface<GnmiEventPtr>> writer) = 0;

  // Unregisters the previously registered event notify writer after calling
  // RegisterEventNotifyWriter().
  virtual ::util::Status UnregisterEventNotifyWriter() = 0;

  // A generic method to retrieve a value specified by 'request'.
  // All types of data that can be retrieved using this method are defined in
  // common.proto
  virtual ::util::Status RetrieveValue(
      uint64 node_id, const DataRequest& request,
      WriterInterface<DataResponse>* writer,
      std::vector<::util::Status>* details) = 0;

  // A generic method to set a value specified by 'request'. All types of data
  // that can be set using this method are defined in common.proto.
  // The 'request' will be processed in the context of the node identified by
  // 'node_id' and the result of each sub-request that is part of the 'request'
  // will be stored in 'details'. The order of statuses in 'details' is the
  // same as they are present in 'request'.
  virtual ::util::Status SetValue(uint64 node_id, const SetRequest& request,
                                  std::vector<::util::Status>* details) = 0;

  // Runs state consistency checks for all internal modules. This generally
  // entails a comparison of software and hardware state. It is guaranteed that
  // the switch configuration will not change during the check. Returns a vector
  // of error messages from the internal modules which will be empty if state is
  // consistent. Returns an error status if any failure occurs in the process of
  // verification.
  //
  // TODO(unknown): Explore the possibility of adding an argument to this
  // call to specify an aspect/multiple aspects of state to verify.
  virtual ::util::StatusOr<std::vector<std::string>> VerifyState() = 0;

 protected:
  // Default constructor. To be called by the Mock class instance only.
  SwitchInterface() {}
};

}  // namespace hal
}  // namespace stratum

#endif  // STRATUM_HAL_LIB_COMMON_SWITCH_INTERFACE_H_
