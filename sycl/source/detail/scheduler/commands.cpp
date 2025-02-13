//===----------- commands.cpp - SYCL commands -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <detail/error_handling/error_handling.hpp>

#include "CL/sycl/access/access.hpp"
#include <CL/cl.h>
#include <CL/sycl/detail/clusm.hpp>
#include <CL/sycl/detail/kernel_desc.hpp>
#include <CL/sycl/detail/memory_manager.hpp>
#include <CL/sycl/detail/stream_impl.hpp>
#include <CL/sycl/sampler.hpp>
#include <detail/context_impl.hpp>
#include <detail/event_impl.hpp>
#include <detail/kernel_impl.hpp>
#include <detail/kernel_info.hpp>
#include <detail/program_manager/program_manager.hpp>
#include <detail/queue_impl.hpp>
#include <detail/scheduler/commands.hpp>
#include <detail/scheduler/scheduler.hpp>

#include <cassert>
#include <string>
#include <vector>

#ifdef __GNUG__
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#endif

#ifdef XPTI_ENABLE_INSTRUMENTATION
#include "xpti_trace_framework.hpp"
#endif

__SYCL_INLINE_NAMESPACE(cl) {
namespace sycl {
namespace detail {
#ifdef XPTI_ENABLE_INSTRUMENTATION
// Global graph for the application
extern xpti::trace_event_data_t *GSYCLGraphEvent;
#endif

#ifdef __GNUG__
struct DemangleHandle {
  char *p;
  DemangleHandle(char *ptr) : p(ptr) {}
  ~DemangleHandle() { std::free(p); }
};
static std::string demangleKernelName(std::string Name) {
  int Status = -1; // some arbitrary value to eliminate the compiler warning
  DemangleHandle result(abi::__cxa_demangle(Name.c_str(), NULL, NULL, &Status));
  return (Status == 0) ? result.p : Name;
}
#else
static std::string demangleKernelName(std::string Name) { return Name; }
#endif

static std::string deviceToString(device Device) {
  if (Device.is_host())
    return "HOST";
  else if (Device.is_cpu())
    return "CPU";
  else if (Device.is_gpu())
    return "GPU";
  else if (Device.is_accelerator())
    return "ACCELERATOR";
  else
    return "UNKNOWN";
}

static std::string accessModeToString(access::mode Mode) {
  switch (Mode) {
  case access::mode::read:
    return "read";
  case access::mode::write:
    return "write";
  case access::mode::read_write:
    return "read_write";
  case access::mode::discard_write:
    return "discard_write";
  case access::mode::discard_read_write:
    return "discard_read_write";
  default:
    return "unknown";
  }
}

#ifdef XPTI_ENABLE_INSTRUMENTATION
// Using the command group type to create node types for the asynchronous task
// graph modeling
static std::string commandToNodeType(Command::CommandType Type) {
  switch (Type) {
  case Command::CommandType::RUN_CG:
    return "command_group_node";
  case Command::CommandType::COPY_MEMORY:
    return "memory_transfer_node";
  case Command::CommandType::ALLOCA:
    return "memory_allocation_node";
  case Command::CommandType::ALLOCA_SUB_BUF:
    return "sub_buffer_creation_node";
  case Command::CommandType::RELEASE:
    return "memory_deallocation_node";
  case Command::CommandType::MAP_MEM_OBJ:
    return "memory_transfer_node";
  case Command::CommandType::UNMAP_MEM_OBJ:
    return "memory_transfer_node";
  case Command::CommandType::UPDATE_REQUIREMENT:
    return "host_acc_create_buffer_lock_node";
  case Command::CommandType::EMPTY_TASK:
    return "host_acc_destroy_buffer_release_node";
  default:
    return "unknown_node";
  }
}

// Using the names being generated and the string are subject to change to
// something more meaningful to end-users as this will be visible in analysis
// tools that subscribe to this data
static std::string commandToName(Command::CommandType Type) {
  switch (Type) {
  case Command::CommandType::RUN_CG:
    return "Command Group Action";
  case Command::CommandType::COPY_MEMORY:
    return "Memory Transfer (Copy)";
  case Command::CommandType::ALLOCA:
    return "Memory Allocation";
  case Command::CommandType::ALLOCA_SUB_BUF:
    return "Sub Buffer Creation";
  case Command::CommandType::RELEASE:
    return "Memory Deallocation";
  case Command::CommandType::MAP_MEM_OBJ:
    return "Memory Transfer (Map)";
  case Command::CommandType::UNMAP_MEM_OBJ:
    return "Memory Transfer (Unmap)";
  case Command::CommandType::UPDATE_REQUIREMENT:
    return "Host Accessor Creation/Buffer Lock";
  case Command::CommandType::EMPTY_TASK:
    return "Host Accessor Destruction/Buffer Lock Release";
  default:
    return "Unknown Action";
  }
}
#endif

static std::vector<RT::PiEvent>
getPiEvents(const std::vector<EventImplPtr> &EventImpls) {
  std::vector<RT::PiEvent> RetPiEvents;
  for (auto &EventImpl : EventImpls)
    RetPiEvents.push_back(EventImpl->getHandleRef());
  return RetPiEvents;
}

void EventCompletionClbk(RT::PiEvent, pi_int32, void *data) {
  // TODO: Handle return values. Store errors to async handler.
  EventImplPtr *Event = (reinterpret_cast<EventImplPtr *>(data));
  RT::PiEvent &EventHandle = (*Event)->getHandleRef();
  const detail::plugin &Plugin = (*Event)->getPlugin();
  Plugin.call<PiApiKind::piEventSetStatus>(EventHandle, PI_EVENT_COMPLETE);
  delete (Event);
}

// Method prepares PI event's from list sycl::event's
std::vector<EventImplPtr> Command::prepareEvents(ContextImplPtr Context) {
  std::vector<EventImplPtr> Result;
  std::vector<EventImplPtr> GlueEvents;
  for (EventImplPtr &DepEvent : MDepsEvents) {
    // Async work is not supported for host device.
    if (DepEvent->is_host()) {
      DepEvent->waitInternal();
      continue;
    }
    // The event handle can be null in case of, for example, alloca command,
    // which is currently synchrounious, so don't generate OpenCL event.
    if (DepEvent->getHandleRef() == nullptr) {
      continue;
    }
    ContextImplPtr DepEventContext = DepEvent->getContextImpl();

    // If contexts don't match - connect them using user event
    if (DepEventContext != Context && !Context->is_host()) {
      EventImplPtr GlueEvent(new detail::event_impl());
      GlueEvent->setContextImpl(Context);
      EventImplPtr *GlueEventCopy =
          new EventImplPtr(GlueEvent); // To increase the reference count by 1.

      RT::PiEvent &GlueEventHandle = GlueEvent->getHandleRef();
      auto Plugin = Context->getPlugin();
      auto DepPlugin = DepEventContext->getPlugin();
      // Add an event on the current context that
      // is triggered when the DepEvent is complete
      Plugin.call<PiApiKind::piEventCreate>(Context->getHandleRef(),
                                            &GlueEventHandle);

      DepPlugin.call<PiApiKind::piEventSetCallback>(
          DepEvent->getHandleRef(), PI_EVENT_COMPLETE, EventCompletionClbk,
          /*void *data=*/(GlueEventCopy));
      GlueEvents.push_back(GlueEvent);
      Result.push_back(std::move(GlueEvent));
      continue;
    }
    Result.push_back(DepEvent);
  }
  MDepsEvents.insert(MDepsEvents.end(), GlueEvents.begin(), GlueEvents.end());
  return Result;
}

void Command::waitForEvents(QueueImplPtr Queue,
                            std::vector<EventImplPtr> &EventImpls,
                            RT::PiEvent &Event) {

  if (!EventImpls.empty()) {
    std::vector<RT::PiEvent> RawEvents = getPiEvents(EventImpls);
    if (Queue->is_host()) {
      const detail::plugin &Plugin = EventImpls[0]->getPlugin();
      Plugin.call<PiApiKind::piEventsWait>(RawEvents.size(), &RawEvents[0]);
    } else {
      const detail::plugin &Plugin = Queue->getPlugin();
      Plugin.call<PiApiKind::piEnqueueEventsWait>(
          Queue->getHandleRef(), RawEvents.size(), &RawEvents[0], &Event);
    }
  }
}

Command::Command(CommandType Type, QueueImplPtr Queue)
    : MQueue(std::move(Queue)), MType(Type), MEnqueued(false) {
  MEvent.reset(new detail::event_impl(MQueue));
  MEvent->setCommand(this);
  MEvent->setContextImpl(detail::getSyclObjImpl(MQueue->get_context()));

#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Obtain the stream ID so all commands can emit traces to that stream
  MStreamID = xptiRegisterStream(SYCL_STREAM_NAME);
#endif
}

void Command::emitInstrumentationDataProxy() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  emitInstrumentationData();
#endif
}

/// Method takes in void * for the address as adding a template function to
/// the command group object maybe undesirable.
/// @param Cmd The command object of the source of the edge
/// @param ObjAddr The address that defines the edge dependency; it is the event
/// address when the edge is for an event and a memory object address if it is
/// due to an accessor
/// @param Prefix Contains "event" if the dependency is an edge and contains the
/// access mode to the buffer if it is due to an accessor
/// @param IsCommand True if the dependency has a command object as the source,
/// false otherwise
void Command::emitEdgeEventForCommandDependence(Command *Cmd, void *ObjAddr,
                                                const string_class &Prefix,
                                                bool IsCommand) {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  // Bail early if either the source or the target node for the given dependency
  // is undefined or NULL
  if (!(xptiTraceEnabled() && MTraceEvent && Cmd && Cmd->MTraceEvent))
    return;
  // If all the information we need for creating an edge event is available,
  // then go ahead with creating it; if not, bail early!
  xpti::utils::StringHelper SH;
  std::string AddressStr = SH.addressAsString<void *>(ObjAddr);
  std::string TypeString = SH.nameWithAddressString(Prefix, AddressStr);
  // Create an edge with the dependent buffer address for which a command
  // object has been created as one of the properties of the edge
  xpti::payload_t Payload(TypeString.c_str(), MAddress);
  uint64_t EdgeInstanceNo;
  xpti_td *EdgeEvent =
      xptiMakeEvent(TypeString.c_str(), &Payload, xpti::trace_graph_event,
                    xpti_at::active, &EdgeInstanceNo);
  if (EdgeEvent) {
    xpti_td *SrcEvent = static_cast<xpti_td *>(Cmd->MTraceEvent);
    xpti_td *TgtEvent = static_cast<xpti_td *>(MTraceEvent);
    EdgeEvent->source_id = SrcEvent->unique_id;
    EdgeEvent->target_id = TgtEvent->unique_id;
    if (IsCommand) {
      xptiAddMetadata(EdgeEvent, "access_mode", TypeString.c_str());
      xptiAddMetadata(EdgeEvent, "memory_object", AddressStr.c_str());
    } else {
      xptiAddMetadata(EdgeEvent, "event", TypeString.c_str());
    }
    xptiNotifySubscribers(MStreamID, xpti::trace_edge_create,
                          detail::GSYCLGraphEvent, EdgeEvent, EdgeInstanceNo,
                          nullptr);
  }
  // General comment - None of these are serious errors as the instrumentation
  // layer MUST be tolerant of errors. If we need to let the end user know, we
  // throw exceptions in the future
#endif
}

/// Creates an edge when the dependency is due to an event.
/// @param Cmd The command object of the source of the edge
/// @param PiEventAddr The address that defines the edge dependency, which in
/// this case is an event
void Command::emitEdgeEventForEventDependence(Command *Cmd,
                                              RT::PiEvent &PiEventAddr) {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  // If we have failed to create an event to represent the Command, then we
  // cannot emit an edge event. Bail early!
  if (!(xptiTraceEnabled() && MTraceEvent))
    return;

  if (Cmd && Cmd->MTraceEvent) {
    // If the event is associated with a command, we use this command's trace
    // event as the source of edge, hence modeling the control flow
    emitEdgeEventForCommandDependence(Cmd, (void *)PiEventAddr, "Event", false);
    return;
  }
  if (PiEventAddr) {
    xpti::utils::StringHelper SH;
    std::string AddressStr = SH.addressAsString<RT::PiEvent>(PiEventAddr);
    // This is the case when it is a OCL event enqueued by the user or another
    // event is registered by the runtime as a dependency The dependency on
    // this occasion is an OCL event; so we build a virtual node in the graph
    // with the event as the metadata for the node
    std::string NodeName = SH.nameWithAddressString("virtual_node", AddressStr);
    // Node name is "virtual_node[<event_addr>]"
    xpti::payload_t VNPayload(NodeName.c_str(), MAddress);
    uint64_t VNodeInstanceNo;
    xpti_td *NodeEvent =
        xptiMakeEvent(NodeName.c_str(), &VNPayload, xpti::trace_graph_event,
                      xpti_at::active, &VNodeInstanceNo);
    // Emit the virtual node first
    xptiAddMetadata(NodeEvent, "kernel_name", NodeName.c_str());
    xptiNotifySubscribers(MStreamID, xpti::trace_node_create,
                          detail::GSYCLGraphEvent, NodeEvent, VNodeInstanceNo,
                          nullptr);
    // Create a new event for the edge
    std::string EdgeName = SH.nameWithAddressString("Event", AddressStr);
    xpti::payload_t EdgePayload(EdgeName.c_str(), MAddress);
    uint64_t EdgeInstanceNo;
    xpti_td *EdgeEvent =
        xptiMakeEvent(EdgeName.c_str(), &EdgePayload, xpti::trace_graph_event,
                      xpti_at::active, &EdgeInstanceNo);
    if (EdgeEvent && NodeEvent) {
      // Source node represents the event and this event needs to be completed
      // before target node can execute
      xpti_td *TgtEvent = static_cast<xpti_td *>(MTraceEvent);
      EdgeEvent->source_id = NodeEvent->unique_id;
      EdgeEvent->target_id = TgtEvent->unique_id;
      xptiAddMetadata(EdgeEvent, "event", EdgeName.c_str());
      xptiNotifySubscribers(MStreamID, xpti::trace_edge_create,
                            detail::GSYCLGraphEvent, EdgeEvent, EdgeInstanceNo,
                            nullptr);
    }
    return;
  }
#endif
}

uint64_t Command::makeTraceEventProlog(void *MAddress) {
  uint64_t CommandInstanceNo = 0;
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return CommandInstanceNo;

  MTraceEventPrologComplete = true;
  // Setup the member variables with information needed for event notification
  MCommandNodeType = commandToNodeType(MType);
  MCommandName = commandToName(MType);
  xpti::utils::StringHelper SH;
  MAddressString = SH.addressAsString<void *>(MAddress);
  std::string CommandString =
      SH.nameWithAddressString(MCommandName, MAddressString);

  xpti::payload_t p(CommandString.c_str(), MAddress);
  xpti_td *CmdTraceEvent =
      xptiMakeEvent(CommandString.c_str(), &p, xpti::trace_graph_event,
                    xpti_at::active, &CommandInstanceNo);
  MInstanceID = CommandInstanceNo;
  if (CmdTraceEvent) {
    MTraceEvent = (void *)CmdTraceEvent;
    // If we are seeing this event again, then the instance ID will be greater
    // than 1; in this case, we must skip sending a notification to create a
    // node as this node has already been created. We return this value so the
    // epilog method can be called selectively.
    MFirstInstance = (CommandInstanceNo == 1);
  }
#endif
  return CommandInstanceNo;
}

void Command::makeTraceEventEpilog() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!(xptiTraceEnabled() && MTraceEvent))
    return;
  assert(MTraceEventPrologComplete);
  xptiNotifySubscribers(MStreamID, xpti::trace_node_create,
                        detail::GSYCLGraphEvent,
                        static_cast<xpti_td *>(MTraceEvent), MInstanceID,
                        static_cast<const void *>(MCommandNodeType.c_str()));
#endif
}

void Command::addDep(DepDesc NewDep) {
  if (NewDep.MDepCommand)
    MDepsEvents.push_back(NewDep.MDepCommand->getEvent());
  MDeps.push_back(NewDep);
#ifdef XPTI_ENABLE_INSTRUMENTATION
  emitEdgeEventForCommandDependence(
      NewDep.MDepCommand, (void *)NewDep.MDepRequirement->MSYCLMemObj,
      accessModeToString(NewDep.MDepRequirement->MAccessMode), true);
#endif
}

void Command::addDep(EventImplPtr Event) {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  // We need this for just the instrumentation, so guarding it will prevent
  // unused variable warnings when instrumentation is turned off
  Command *Cmd = (Command *)Event->getCommand();
  RT::PiEvent &PiEventAddr = Event->getHandleRef();
  // Now make an edge for the dependent event
  emitEdgeEventForEventDependence(Cmd, PiEventAddr);
#endif

  MDepsEvents.push_back(std::move(Event));
}

void Command::emitEnqueuedEventSignal(RT::PiEvent &PiEventAddr) {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!(xptiTraceEnabled() && MTraceEvent && PiEventAddr))
    return;
  // Asynchronous call, so send a signal with the event information as
  // user_data
  xptiNotifySubscribers(MStreamID, xpti::trace_signal, detail::GSYCLGraphEvent,
                        static_cast<xpti_td *>(MTraceEvent), MInstanceID,
                        (void *)PiEventAddr);
#endif
}

void Command::emitInstrumentation(uint16_t Type, const char *Txt) {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!(xptiTraceEnabled() && MTraceEvent))
    return;
  // Trace event notifier that emits a Type event
  xptiNotifySubscribers(MStreamID, Type, detail::GSYCLGraphEvent,
                        static_cast<xpti_td *>(MTraceEvent), MInstanceID,
                        static_cast<const void *>(Txt));
#endif
}

bool Command::enqueue(EnqueueResultT &EnqueueResult, BlockingT Blocking) {
  // Exit if already enqueued
  if (MEnqueued)
    return true;

  // If the command is blocked from enqueueing
  if (MIsBlockable && !MCanEnqueue) {
    // Exit if enqueue type is not blocking
    if (!Blocking) {
      EnqueueResult = EnqueueResultT(EnqueueResultT::SyclEnqueueBlocked, this);
      return false;
    }
    static bool ThrowOnBlock = getenv("SYCL_THROW_ON_BLOCK") != nullptr;
    if (ThrowOnBlock)
      throw sycl::runtime_error(
          std::string("Waiting for blocked command. Block reason: ") +
              std::string(MBlockReason),
          PI_INVALID_OPERATION);

#ifdef XPTI_ENABLE_INSTRUMENTATION
    // Scoped trace event notifier that emits a barrier begin and barrier end
    // event, which models the barrier while enqueuing along with the blocked
    // reason, as determined by the scheduler
    std::string Info = "enqueue.barrier[";
    Info += std::string(MBlockReason) + "]";
    emitInstrumentation(xpti::trace_barrier_begin, Info.c_str());
#endif

    // Wait if blocking
    while (!MCanEnqueue)
      ;
#ifdef XPTI_ENABLE_INSTRUMENTATION
    emitInstrumentation(xpti::trace_barrier_end, Info.c_str());
#endif
  }

  std::lock_guard<std::mutex> Lock(MEnqueueMtx);

  // Exit if the command is already enqueued
  if (MEnqueued)
    return true;

#ifdef XPTI_ENABLE_INSTRUMENTATION
  emitInstrumentation(xpti::trace_task_begin, nullptr);
#endif

  cl_int Res = enqueueImp();

  if (CL_SUCCESS != Res)
    EnqueueResult =
        EnqueueResultT(EnqueueResultT::SyclEnqueueFailed, this, Res);
  else
    // Consider the command is successfully enqueued if return code is
    // CL_SUCCESS
    MEnqueued = true;

  // Emit this correlation signal before the task end
  emitEnqueuedEventSignal(MEvent->getHandleRef());
#ifdef XPTI_ENABLE_INSTRUMENTATION
  emitInstrumentation(xpti::trace_task_end, nullptr);
#endif
  return static_cast<bool>(MEnqueued);
}

void Command::resolveReleaseDependencies(std::set<Command *> &DepList) {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  assert(MType == CommandType::RELEASE && "Expected release command");
  if (!MTraceEvent)
    return;
  // The current command is the target node for all dependencies as the source
  // nodes have to be completed first before the current node can begin to
  // execute; these edges model control flow
  xpti_td *TgtTraceEvent = static_cast<xpti_td *>(MTraceEvent);
  // We have all the Commands that must be completed before the release command
  // can be enqueued; here we'll find the command that is an Alloca with the
  // same SYCLMemObject address and create a dependency line (edge) between them
  // in our sematic modeling
  for (auto &Item : DepList) {
    if (Item->MTraceEvent && Item->MAddress == MAddress) {
      xpti::utils::StringHelper SH;
      std::string AddressStr = SH.addressAsString<void *>(MAddress);
      std::string TypeString =
          "Edge:" + SH.nameWithAddressString(commandToName(MType), AddressStr);

      // Create an edge with the dependent buffer address being one of the
      // properties of the edge
      xpti::payload_t p(TypeString.c_str(), MAddress);
      uint64_t EdgeInstanceNo;
      xpti_td *EdgeEvent =
          xptiMakeEvent(TypeString.c_str(), &p, xpti::trace_graph_event,
                        xpti_at::active, &EdgeInstanceNo);
      if (EdgeEvent) {
        xpti_td *SrcTraceEvent = static_cast<xpti_td *>(Item->MTraceEvent);
        EdgeEvent->target_id = TgtTraceEvent->unique_id;
        EdgeEvent->source_id = SrcTraceEvent->unique_id;
        xptiAddMetadata(EdgeEvent, "memory_object", AddressStr.c_str());
        xptiNotifySubscribers(MStreamID, xpti::trace_edge_create,
                              detail::GSYCLGraphEvent, EdgeEvent,
                              EdgeInstanceNo, nullptr);
      }
    }
  }
#endif
}

AllocaCommandBase::AllocaCommandBase(CommandType Type, QueueImplPtr Queue,
                                     Requirement Req,
                                     AllocaCommandBase *LinkedAllocaCmd)
    : Command(Type, Queue), MLinkedAllocaCmd(LinkedAllocaCmd),
      MIsLeaderAlloca(nullptr == LinkedAllocaCmd), MRequirement(std::move(Req)),
      MReleaseCmd(Queue, this) {
  MRequirement.MAccessMode = access::mode::read_write;
  emitInstrumentationDataProxy();
}

void AllocaCommandBase::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MRequirement.MSYCLMemObj;
  makeTraceEventProlog(MAddress);
  // Set the relevant meta data properties for this command
  if (MTraceEvent && MFirstInstance) {
    xpti_td *TE = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(TE, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(TE, "memory_object", MAddressString.c_str());
  }
#endif
}

AllocaCommand::AllocaCommand(QueueImplPtr Queue, Requirement Req,
                             bool InitFromUserData,
                             AllocaCommandBase *LinkedAllocaCmd)
    : AllocaCommandBase(CommandType::ALLOCA, std::move(Queue), std::move(Req),
                        LinkedAllocaCmd),
      MInitFromUserData(InitFromUserData) {
  // Node event must be created before the dependent edge is added to this node,
  // so this call must be before the addDep() call.
  emitInstrumentationDataProxy();
  addDep(DepDesc(nullptr, getRequirement(), this));
}

void AllocaCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;

  // Only if it is the first event, we emit a node create event
  if (MFirstInstance) {
    makeTraceEventEpilog();
  }
#endif
}

cl_int AllocaCommand::enqueueImp() {
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));

  RT::PiEvent &Event = MEvent->getHandleRef();

  void *HostPtr = nullptr;
  if (!MIsLeaderAlloca) {

    if (MQueue->is_host()) {
      // Do not need to make allocation if we have a linked device allocation
      Command::waitForEvents(MQueue, EventImpls, Event);
      return CL_SUCCESS;
    }
    HostPtr = MLinkedAllocaCmd->getMemAllocation();
  }
  // TODO: Check if it is correct to use std::move on stack variable and
  // delete it RawEvents below.
  MMemAllocation = MemoryManager::allocate(
      detail::getSyclObjImpl(MQueue->get_context()), getSYCLMemObj(),
      MInitFromUserData, HostPtr, std::move(EventImpls), Event);
  return CL_SUCCESS;
}

void AllocaCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#FFD28A\", label=\"";

  Stream << "ID = " << this << "\\n";
  Stream << "ALLOCA ON " << deviceToString(MQueue->get_device()) << "\\n";
  Stream << " MemObj : " << this->MRequirement.MSYCLMemObj << "\\n";
  Stream << " Link : " << this->MLinkedAllocaCmd << "\\n";
  Stream << "\"];" << std::endl;


  for (const auto &Dep : MDeps) {
    if (Dep.MDepCommand == nullptr)
      continue;
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

AllocaSubBufCommand::AllocaSubBufCommand(QueueImplPtr Queue, Requirement Req,
                                         AllocaCommandBase *ParentAlloca)
    : AllocaCommandBase(CommandType::ALLOCA_SUB_BUF, std::move(Queue),
                        std::move(Req),
                        /*LinkedAllocaCmd*/ nullptr),
      MParentAlloca(ParentAlloca) {
  // Node event must be created before the dependent edge
  // is added to this node, so this call must be before
  // the addDep() call.
  emitInstrumentationDataProxy();
  addDep(DepDesc(MParentAlloca, getRequirement(), MParentAlloca));
}

void AllocaSubBufCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;

  // Only if it is the first event, we emit a node create event and any meta
  // data that is available for the command
  if (MFirstInstance) {
    xpti_td *TE = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(TE, "offset",
                    std::to_string(this->MRequirement.MOffsetInBytes).c_str());
    std::string range = std::to_string(this->MRequirement.MAccessRange[0]) +
                        "-" +
                        std::to_string(this->MRequirement.MAccessRange[1]);
    xptiAddMetadata(TE, "access_range", range.c_str());
    makeTraceEventEpilog();
  }
#endif
}

cl_int AllocaSubBufCommand::enqueueImp() {
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));
  RT::PiEvent &Event = MEvent->getHandleRef();

  MMemAllocation = MemoryManager::allocateMemSubBuffer(
      detail::getSyclObjImpl(MQueue->get_context()),
      MParentAlloca->getMemAllocation(), MRequirement.MElemSize,
      MRequirement.MOffsetInBytes, MRequirement.MAccessRange,
      std::move(EventImpls), Event);
  return CL_SUCCESS;
}

void AllocaSubBufCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#FFD28A\", label=\"";

  Stream << "ID = " << this << "\\n";
  Stream << "ALLOCA SUB BUF ON " << deviceToString(MQueue->get_device())
         << "\\n";
  Stream << " MemObj : " << this->MRequirement.MSYCLMemObj << "\\n";
  Stream << " Offset : " << this->MRequirement.MOffsetInBytes << "\\n";
  Stream << " Access range : " << this->MRequirement.MAccessRange[0] << "\\n";
  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    if (Dep.MDepCommand == nullptr)
      continue;
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

ReleaseCommand::ReleaseCommand(QueueImplPtr Queue, AllocaCommandBase *AllocaCmd)
    : Command(CommandType::RELEASE, std::move(Queue)), MAllocaCmd(AllocaCmd) {
  emitInstrumentationDataProxy();
}

void ReleaseCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MAllocaCmd->getSYCLMemObj();
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *TE = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(TE, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(TE, "allocation_type",
                    commandToName(MAllocaCmd->getType()).c_str());
    makeTraceEventEpilog();
  }
#endif
}

cl_int ReleaseCommand::enqueueImp() {
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));
  std::vector<RT::PiEvent> RawEvents = getPiEvents(EventImpls);
  bool SkipRelease = false;

  // On host side we only allocate memory for full buffers.
  // Thus, deallocating sub buffers leads to double memory freeing.
  SkipRelease |= MQueue->is_host() && MAllocaCmd->getType() == ALLOCA_SUB_BUF;

  const bool CurAllocaIsHost = MAllocaCmd->getQueue()->is_host();
  bool NeedUnmap = false;
  if (MAllocaCmd->MLinkedAllocaCmd) {

    // When releasing one of the "linked" allocations special rules take place:
    // 1. Device allocation should always be released.
    // 2. Host allocation should be released if host allocation is "leader".
    // 3. Device alloca in the pair should be in active state in order to be
    //    correctly released.


    // There is no actual memory allocation if a host alloca command is created
    // being linked to a device allocation.
    SkipRelease |= CurAllocaIsHost && !MAllocaCmd->MIsLeaderAlloca;

    NeedUnmap |= CurAllocaIsHost == MAllocaCmd->MIsActive;
  }

  if (NeedUnmap) {
    const QueueImplPtr &Queue = CurAllocaIsHost
                                    ? MAllocaCmd->MLinkedAllocaCmd->getQueue()
                                    : MAllocaCmd->getQueue();

    EventImplPtr UnmapEventImpl(new event_impl(Queue));
    UnmapEventImpl->setContextImpl(
        detail::getSyclObjImpl(Queue->get_context()));
    RT::PiEvent &UnmapEvent = UnmapEventImpl->getHandleRef();

    void *Src = CurAllocaIsHost
                    ? MAllocaCmd->getMemAllocation()
                    : MAllocaCmd->MLinkedAllocaCmd->getMemAllocation();

    void *Dst = !CurAllocaIsHost
                    ? MAllocaCmd->getMemAllocation()
                    : MAllocaCmd->MLinkedAllocaCmd->getMemAllocation();

    MemoryManager::unmap(MAllocaCmd->getSYCLMemObj(), Dst, Queue, Src,
                         RawEvents, UnmapEvent);

    std::swap(MAllocaCmd->MIsActive, MAllocaCmd->MLinkedAllocaCmd->MIsActive);
    EventImpls.clear();
    EventImpls.push_back(UnmapEventImpl);
  }
  RT::PiEvent &Event = MEvent->getHandleRef();
  if (SkipRelease)
    Command::waitForEvents(MQueue, EventImpls, Event);
  else
    MemoryManager::release(detail::getSyclObjImpl(MQueue->get_context()),
                           MAllocaCmd->getSYCLMemObj(),
                           MAllocaCmd->getMemAllocation(),
                           std::move(EventImpls), Event);

  return CL_SUCCESS;
}

void ReleaseCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#FF827A\", label=\"";

  Stream << "ID = " << this << " ; ";
  Stream << "RELEASE ON " << deviceToString(MQueue->get_device()) << "\\n";
  Stream << " Alloca : " << MAllocaCmd << "\\n";
  Stream << " MemObj : " << MAllocaCmd->getSYCLMemObj() << "\\n";
  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

MapMemObject::MapMemObject(AllocaCommandBase *SrcAllocaCmd, Requirement Req,
                           void **DstPtr, QueueImplPtr Queue)
    : Command(CommandType::MAP_MEM_OBJ, std::move(Queue)),
      MSrcAllocaCmd(SrcAllocaCmd), MSrcReq(std::move(Req)), MDstPtr(DstPtr) {
  emitInstrumentationDataProxy();
}

void MapMemObject::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MSrcAllocaCmd->getSYCLMemObj();
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *TE = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(TE, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(TE, "memory_object", MAddressString.c_str());
    makeTraceEventEpilog();
  }
#endif
}

cl_int MapMemObject::enqueueImp() {
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));
  std::vector<RT::PiEvent> RawEvents = getPiEvents(EventImpls);

  RT::PiEvent &Event = MEvent->getHandleRef();
  *MDstPtr = MemoryManager::map(
      MSrcAllocaCmd->getSYCLMemObj(), MSrcAllocaCmd->getMemAllocation(), MQueue,
      MSrcReq.MAccessMode, MSrcReq.MDims, MSrcReq.MMemoryRange,
      MSrcReq.MAccessRange, MSrcReq.MOffset, MSrcReq.MElemSize,
      std::move(RawEvents), Event);
  return CL_SUCCESS;
}

void MapMemObject::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#77AFFF\", label=\"";

  Stream << "ID = " << this << " ; ";
  Stream << "MAP ON " << deviceToString(MQueue->get_device()) << "\\n";

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

UnMapMemObject::UnMapMemObject(AllocaCommandBase *DstAllocaCmd, Requirement Req,
                               void **SrcPtr, QueueImplPtr Queue)
    : Command(CommandType::UNMAP_MEM_OBJ, std::move(Queue)),
      MDstAllocaCmd(DstAllocaCmd), MDstReq(std::move(Req)), MSrcPtr(SrcPtr) {
  emitInstrumentationDataProxy();
}

void UnMapMemObject::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MDstAllocaCmd->getSYCLMemObj();
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *TE = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(TE, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(TE, "memory_object", MAddressString.c_str());
    makeTraceEventEpilog();
  }
#endif
}

cl_int UnMapMemObject::enqueueImp() {
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));
  std::vector<RT::PiEvent> RawEvents = getPiEvents(EventImpls);

  RT::PiEvent &Event = MEvent->getHandleRef();
  MemoryManager::unmap(MDstAllocaCmd->getSYCLMemObj(),
                       MDstAllocaCmd->getMemAllocation(), MQueue, *MSrcPtr,
                       std::move(RawEvents), Event);
  return CL_SUCCESS;
}

void UnMapMemObject::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#EBC40F\", label=\"";

  Stream << "ID = " << this << " ; ";
  Stream << "UNMAP ON " << deviceToString(MQueue->get_device()) << "\\n";

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

MemCpyCommand::MemCpyCommand(Requirement SrcReq,
                             AllocaCommandBase *SrcAllocaCmd,
                             Requirement DstReq,
                             AllocaCommandBase *DstAllocaCmd,
                             QueueImplPtr SrcQueue, QueueImplPtr DstQueue)
    : Command(CommandType::COPY_MEMORY, std::move(DstQueue)),
      MSrcQueue(SrcQueue), MSrcReq(std::move(SrcReq)),
      MSrcAllocaCmd(SrcAllocaCmd), MDstReq(std::move(DstReq)),
      MDstAllocaCmd(DstAllocaCmd) {
  if (!MSrcQueue->is_host())
    MEvent->setContextImpl(detail::getSyclObjImpl(MSrcQueue->get_context()));

  emitInstrumentationDataProxy();
}

void MemCpyCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MSrcAllocaCmd->getSYCLMemObj();
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *CmdTraceEvent = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(CmdTraceEvent, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(CmdTraceEvent, "memory_object", MAddressString.c_str());
    std::string From = deviceToString(MSrcQueue->get_device());
    std::string To = deviceToString(MQueue->get_device());
    xptiAddMetadata(CmdTraceEvent, "copy_from", From.c_str());
    xptiAddMetadata(CmdTraceEvent, "copy_to", To.c_str());
    makeTraceEventEpilog();
  }
#endif
}

cl_int MemCpyCommand::enqueueImp() {
  std::vector<EventImplPtr> EventImpls;
  QueueImplPtr Queue = MQueue->is_host() ? MSrcQueue : MQueue;
  EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(Queue->get_context()));

  RT::PiEvent &Event = MEvent->getHandleRef();

  auto RawEvents = getPiEvents(EventImpls);

  // Omit copying if mode is discard one.
  // TODO: Handle this at the graph building time by, for example, creating
  // empty node instead of memcpy.
  if (MDstReq.MAccessMode == access::mode::discard_read_write ||
      MDstReq.MAccessMode == access::mode::discard_write ||
      MSrcAllocaCmd->getMemAllocation() == MDstAllocaCmd->getMemAllocation()) {
    Command::waitForEvents(Queue, EventImpls, Event);
  } else {
    MemoryManager::copy(
        MSrcAllocaCmd->getSYCLMemObj(), MSrcAllocaCmd->getMemAllocation(),
        MSrcQueue, MSrcReq.MDims, MSrcReq.MMemoryRange, MSrcReq.MAccessRange,
        MSrcReq.MOffset, MSrcReq.MElemSize, MDstAllocaCmd->getMemAllocation(),
        MQueue, MDstReq.MDims, MDstReq.MMemoryRange, MDstReq.MAccessRange,
        MDstReq.MOffset, MDstReq.MElemSize, std::move(RawEvents), Event);
  }

  return CL_SUCCESS;
}

void MemCpyCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#C7EB15\" label=\"";

  Stream << "ID = " << this << " ; ";
  Stream << "MEMCPY ON " << deviceToString(MQueue->get_device()) << "\\n";
  Stream << "From: " << MSrcAllocaCmd << " is host: " << MSrcQueue->is_host()
         << "\\n";
  Stream << "To: " << MDstAllocaCmd << " is host: " << MQueue->is_host()
         << "\\n";

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

AllocaCommandBase *ExecCGCommand::getAllocaForReq(Requirement *Req) {
  for (const DepDesc &Dep : MDeps) {
    if (Dep.MDepRequirement == Req)
      return Dep.MAllocaCmd;
  }
  throw runtime_error("Alloca for command not found", PI_INVALID_OPERATION);
}

void ExecCGCommand::flushStreams() {
  assert(MCommandGroup->getType() == CG::KERNEL && "Expected kernel");
  for (auto StreamImplPtr :
       ((CGExecKernel *)MCommandGroup.get())->getStreams()) {
    StreamImplPtr->flush();
  }
}

cl_int UpdateHostRequirementCommand::enqueueImp() {
  std::vector<EventImplPtr> EventImpls;
  EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));
  RT::PiEvent &Event = MEvent->getHandleRef();
  Command::waitForEvents(MQueue, EventImpls, Event);

  assert(MSrcAllocaCmd && "Expected valid alloca command");
  assert(MSrcAllocaCmd->getMemAllocation() && "Expected valid source pointer");
  assert(MDstPtr && "Expected valid target pointer");
  *MDstPtr = MSrcAllocaCmd->getMemAllocation();
  return CL_SUCCESS;
}

void UpdateHostRequirementCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#f1337f\", label=\"";

  Stream << "ID = " << this << "\\n";
  Stream << "UPDATE REQ ON " << deviceToString(MQueue->get_device()) << "\\n";
  bool IsReqOnBuffer =
      MDstReq.MSYCLMemObj->getType() == SYCLMemObjI::MemObjType::BUFFER;
  Stream << "TYPE: " << (IsReqOnBuffer ? "Buffer" : "Image") << "\\n";
  if (IsReqOnBuffer)
    Stream << "Is sub buffer: " << std::boolalpha << MDstReq.MIsSubBuffer
           << "\\n";

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MAllocaCmd->getSYCLMemObj() << " \" ]"
           << std::endl;
  }
}

MemCpyCommandHost::MemCpyCommandHost(Requirement SrcReq,
                                     AllocaCommandBase *SrcAllocaCmd,
                                     Requirement DstReq, void **DstPtr,
                                     QueueImplPtr SrcQueue,
                                     QueueImplPtr DstQueue)
    : Command(CommandType::COPY_MEMORY, std::move(DstQueue)),
      MSrcQueue(SrcQueue), MSrcReq(std::move(SrcReq)),
      MSrcAllocaCmd(SrcAllocaCmd), MDstReq(std::move(DstReq)), MDstPtr(DstPtr) {
  if (!MSrcQueue->is_host())
    MEvent->setContextImpl(detail::getSyclObjImpl(MSrcQueue->get_context()));

  emitInstrumentationDataProxy();
}

void MemCpyCommandHost::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MSrcAllocaCmd->getSYCLMemObj();
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *CmdTraceEvent = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(CmdTraceEvent, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(CmdTraceEvent, "memory_object", MAddressString.c_str());
    std::string From = deviceToString(MSrcQueue->get_device());
    std::string To = deviceToString(MQueue->get_device());
    xptiAddMetadata(CmdTraceEvent, "copy_from", From.c_str());
    xptiAddMetadata(CmdTraceEvent, "copy_to", To.c_str());
    makeTraceEventEpilog();
  }
#endif
}

cl_int MemCpyCommandHost::enqueueImp() {
  QueueImplPtr Queue = MQueue->is_host() ? MSrcQueue : MQueue;
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(Queue->get_context()));
  std::vector<RT::PiEvent> RawEvents = getPiEvents(EventImpls);

  RT::PiEvent &Event = MEvent->getHandleRef();
  // Omit copying if mode is discard one.
  // TODO: Handle this at the graph building time by, for example, creating
  // empty node instead of memcpy.
  if (MDstReq.MAccessMode == access::mode::discard_read_write ||
      MDstReq.MAccessMode == access::mode::discard_write) {
    Command::waitForEvents(Queue, EventImpls, Event);
    return CL_SUCCESS;
  }

  MemoryManager::copy(
      MSrcAllocaCmd->getSYCLMemObj(), MSrcAllocaCmd->getMemAllocation(),
      MSrcQueue, MSrcReq.MDims, MSrcReq.MMemoryRange, MSrcReq.MAccessRange,
      MSrcReq.MOffset, MSrcReq.MElemSize, *MDstPtr, MQueue, MDstReq.MDims,
      MDstReq.MMemoryRange, MDstReq.MAccessRange, MDstReq.MOffset,
      MDstReq.MElemSize, std::move(RawEvents), Event);
  return CL_SUCCESS;
}

EmptyCommand::EmptyCommand(QueueImplPtr Queue, Requirement Req)
    : Command(CommandType::EMPTY_TASK, std::move(Queue)),
      MRequirement(std::move(Req)) {

  emitInstrumentationDataProxy();
}

void EmptyCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MRequirement.MSYCLMemObj;
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *CmdTraceEvent = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(CmdTraceEvent, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(CmdTraceEvent, "memory_object", MAddressString.c_str());
    makeTraceEventEpilog();
  }
#endif
}

void EmptyCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#8d8f29\", label=\"";

  Stream << "ID = " << this << "\\n";
  Stream << "EMPTY NODE"
         << "\\n";

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

void MemCpyCommandHost::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#B6A2EB\", label=\"";

  Stream << "ID = " << this << "\\n";
  Stream << "MEMCPY HOST ON " << deviceToString(MQueue->get_device()) << "\\n";

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

UpdateHostRequirementCommand::UpdateHostRequirementCommand(
    QueueImplPtr Queue, Requirement Req, AllocaCommandBase *SrcAllocaCmd,
    void **DstPtr)
    : Command(CommandType::UPDATE_REQUIREMENT, std::move(Queue)),
      MSrcAllocaCmd(SrcAllocaCmd), MDstReq(std::move(Req)), MDstPtr(DstPtr) {

  emitInstrumentationDataProxy();
}

void UpdateHostRequirementCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  MAddress = MSrcAllocaCmd->getSYCLMemObj();
  makeTraceEventProlog(MAddress);

  if (MFirstInstance) {
    xpti_td *CmdTraceEvent = static_cast<xpti_td *>(MTraceEvent);
    xptiAddMetadata(CmdTraceEvent, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    xptiAddMetadata(CmdTraceEvent, "memory_object", MAddressString.c_str());
    makeTraceEventEpilog();
  }
#endif
}

static std::string cgTypeToString(detail::CG::CGTYPE Type) {
  switch (Type) {
  case detail::CG::KERNEL:
    return "Kernel";
    break;
  case detail::CG::UPDATE_HOST:
    return "update_host";
    break;
  case detail::CG::FILL:
    return "fill";
    break;
  case detail::CG::COPY_ACC_TO_ACC:
    return "copy acc to acc";
    break;
  case detail::CG::COPY_ACC_TO_PTR:
    return "copy acc to ptr";
    break;
  case detail::CG::COPY_PTR_TO_ACC:
    return "copy ptr to acc";
    break;
  case detail::CG::COPY_USM:
    return "copy usm";
    break;
  case detail::CG::FILL_USM:
    return "fill usm";
    break;
  case detail::CG::PREFETCH_USM:
    return "prefetch usm";
    break;
  default:
    return "unknown";
    break;
  }
}

ExecCGCommand::ExecCGCommand(std::unique_ptr<detail::CG> CommandGroup,
                             QueueImplPtr Queue)
    : Command(CommandType::RUN_CG, std::move(Queue)),
      MCommandGroup(std::move(CommandGroup)) {

  emitInstrumentationDataProxy();
}

void ExecCGCommand::emitInstrumentationData() {
#ifdef XPTI_ENABLE_INSTRUMENTATION
  if (!xptiTraceEnabled())
    return;
  // Create a payload with the command name and an event using this payload to
  // emit a node_create
  bool HasSourceInfo = false;
  std::string KernelName, FromSource;
  switch (MCommandGroup->getType()) {
  case detail::CG::KERNEL: {
    auto KernelCG =
        reinterpret_cast<detail::CGExecKernel *>(MCommandGroup.get());

    if (KernelCG->MSyclKernel && KernelCG->MSyclKernel->isCreatedFromSource()) {
      FromSource = "true";
      pi_kernel KernelHandle = KernelCG->MSyclKernel->getHandleRef();
      MAddress = KernelHandle;
      KernelName = MCommandGroup->MFunctionName;
    } else {
      FromSource = "false";
      KernelName = demangleKernelName(KernelCG->getKernelName());
    }
  } break;
  default:
    KernelName = cgTypeToString(MCommandGroup->getType());
    break;
  }
  std::string CommandType = commandToNodeType(MType);
  //  Get source file, line number information from the CommandGroup object
  //  and create payload using name, address, and source info
  //
  //  On Windows, since the support for builtin functions is not available in
  //  MSVC, the MFileName, MLine will be set to nullptr and "0" respectively.
  //  Handle this condition explicitly here.
  xpti::payload_t Payload;
  if (!MCommandGroup->MFileName.empty()) {
    // File name has a valid string
    Payload =
        xpti::payload_t(KernelName.c_str(), MCommandGroup->MFileName.c_str(),
                        MCommandGroup->MLine, MCommandGroup->MColumn, MAddress);
    HasSourceInfo = true;
  } else if (MAddress) {
    // We have a valid function name and an address
    Payload = xpti::payload_t(KernelName.c_str(), MAddress);
  } else {
    // In any case, we will have a valid function name and we'll use that to
    // create the hash
    Payload = xpti::payload_t(KernelName.c_str());
  }

  uint64_t CGKernelInstanceNo;
  // Create event using the payload
  xpti_td *CmdTraceEvent =
      xptiMakeEvent("ExecCG", &Payload, xpti::trace_graph_event,
                    xpti::trace_activity_type_t::active, &CGKernelInstanceNo);

  if (CmdTraceEvent) {
    MInstanceID = CGKernelInstanceNo;
    MTraceEvent = (void *)CmdTraceEvent;
    // If we are seeing this event again, then the instance ID will be greater
    // than 1; in this case, we will skip sending a notification to create a
    // node as this node has already been created.
    if (CGKernelInstanceNo > 1)
      return;

    xptiAddMetadata(CmdTraceEvent, "sycl_device",
                    deviceToString(MQueue->get_device()).c_str());
    if (!KernelName.empty()) {
      xptiAddMetadata(CmdTraceEvent, "kernel_name", KernelName.c_str());
    }
    if (!FromSource.empty()) {
      xptiAddMetadata(CmdTraceEvent, "from_source", FromSource.c_str());
    }
    if (HasSourceInfo) {
      xptiAddMetadata(CmdTraceEvent, "sym_function_name", KernelName.c_str());
      xptiAddMetadata(CmdTraceEvent, "sym_source_file_name",
                      MCommandGroup->MFileName.c_str());
      xptiAddMetadata(CmdTraceEvent, "sym_line_no",
                      std::to_string(MCommandGroup->MLine).c_str());
    }

    xptiNotifySubscribers(MStreamID, xpti::trace_node_create,
                          detail::GSYCLGraphEvent, CmdTraceEvent,
                          CGKernelInstanceNo,
                          static_cast<const void *>(CommandType.c_str()));
  }
#endif
}

void ExecCGCommand::printDot(std::ostream &Stream) const {
  Stream << "\"" << this << "\" [style=filled, fillcolor=\"#AFFF82\", label=\"";

  Stream << "ID = " << this << "\\n";
  Stream << "EXEC CG ON " << deviceToString(MQueue->get_device()) << "\\n";

  switch (MCommandGroup->getType()) {
  case detail::CG::KERNEL: {
    auto KernelCG =
        reinterpret_cast<detail::CGExecKernel *>(MCommandGroup.get());
    Stream << "Kernel name: ";
    if (KernelCG->MSyclKernel && KernelCG->MSyclKernel->isCreatedFromSource())
      Stream << "created from source";
    else
      Stream << demangleKernelName(KernelCG->getKernelName());
    Stream << "\\n";
    break;
  }
  default:
    Stream << "CG type: " << cgTypeToString(MCommandGroup->getType()) << "\\n";
    break;
  }

  Stream << "\"];" << std::endl;

  for (const auto &Dep : MDeps) {
    Stream << "  \"" << this << "\" -> \"" << Dep.MDepCommand << "\""
           << " [ label = \"Access mode: "
           << accessModeToString(Dep.MDepRequirement->MAccessMode) << "\\n"
           << "MemObj: " << Dep.MDepRequirement->MSYCLMemObj << " \" ]"
           << std::endl;
  }
}

// SYCL has a parallel_for_work_group variant where the only NDRange
// characteristics set by a user is the number of work groups. This does not map
// to the OpenCL clEnqueueNDRangeAPI, which requires global work size to be set
// as well. This function determines local work size based on the device
// characteristics and the number of work groups requested by the user, then
// calculates the global work size.
// SYCL specification (from 4.8.5.3):
// The member function handler::parallel_for_work_group is parameterized by the
// number of work - groups, such that the size of each group is chosen by the
// runtime, or by the number of work - groups and number of work - items for
// users who need more control.
static void adjustNDRangePerKernel(NDRDescT &NDR, RT::PiKernel Kernel,
                                   const device_impl &DeviceImpl) {
  if (NDR.GlobalSize[0] != 0)
    return; // GlobalSize is set - no need to adjust
  // check the prerequisites:
  assert(NDR.NumWorkGroups[0] != 0 && NDR.LocalSize[0] == 0);
  // TODO might be good to cache this info together with the kernel info to
  // avoid get_kernel_work_group_info on every kernel run
  range<3> WGSize = get_kernel_work_group_info<
      range<3>, cl::sycl::info::kernel_work_group::compile_work_group_size>::
      get(Kernel, DeviceImpl.getHandleRef(), DeviceImpl.getPlugin());

  if (WGSize[0] == 0) {
    // kernel does not request specific workgroup shape - set one
    // TODO maximum work group size as the local size might not be the best
    //      choice for CPU or FPGA devices
    size_t WGSize1D = get_kernel_work_group_info<
        size_t, cl::sycl::info::kernel_work_group::work_group_size>::
        get(Kernel, DeviceImpl.getHandleRef(), DeviceImpl.getPlugin());
    assert(WGSize1D != 0);
    // TODO implement better default for 2D/3D case:
    WGSize = {WGSize1D, 1, 1};
  }
  NDR.set(NDR.Dims, nd_range<3>(NDR.NumWorkGroups * WGSize, WGSize));
}

// We have the following mapping between dimensions with SPIRV builtins:
// 1D: id[0] -> x
// 2D: id[0] -> y, id[1] -> x
// 3D: id[0] -> z, id[1] -> y, id[2] -> x
// So in order to ensure the correctness we update all the kernel
// parameters accordingly.
// Initially we keep the order of NDRDescT as it provided by the user, this
// simplifies overall handling and do the reverse only when
// the kernel is enqueued.
static void ReverseRangeDimensionsForKernel(NDRDescT &NDR) {
  if (NDR.Dims > 1) {
    std::swap(NDR.GlobalSize[0], NDR.GlobalSize[NDR.Dims - 1]);
    std::swap(NDR.LocalSize[0], NDR.LocalSize[NDR.Dims - 1]);
    std::swap(NDR.GlobalOffset[0], NDR.GlobalOffset[NDR.Dims - 1]);
  }
}

// The function initialize accessors and calls lambda.
// The function is used as argument to piEnqueueNativeKernel which requires
// that the passed function takes one void* argument.
void DispatchNativeKernel(void *Blob) {
  // First value is a pointer to Corresponding CGExecKernel object.
  CGExecKernel *HostTask = *(CGExecKernel **)Blob;

  // Other value are pointer to the buffers.
  void **NextArg = (void **)Blob + 1;
  for (detail::Requirement *Req : HostTask->MRequirements)
    Req->MData = *(NextArg++);
  HostTask->MHostKernel->call(HostTask->MNDRDesc, nullptr);
}

cl_int ExecCGCommand::enqueueImp() {
  std::vector<EventImplPtr> EventImpls =
      Command::prepareEvents(detail::getSyclObjImpl(MQueue->get_context()));

  auto RawEvents = getPiEvents(EventImpls);

  RT::PiEvent &Event = MEvent->getHandleRef();

  switch (MCommandGroup->getType()) {

  case CG::CGTYPE::UPDATE_HOST: {
    assert(!"Update host should be handled by the Scheduler.");
    throw runtime_error("Update host should be handled by the Scheduler.",
                        PI_INVALID_OPERATION);
  }
  case CG::CGTYPE::COPY_ACC_TO_PTR: {
    CGCopy *Copy = (CGCopy *)MCommandGroup.get();
    Requirement *Req = (Requirement *)Copy->getSrc();
    AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);

    MemoryManager::copy(
        AllocaCmd->getSYCLMemObj(), AllocaCmd->getMemAllocation(), MQueue,
        Req->MDims, Req->MMemoryRange, Req->MAccessRange, Req->MOffset,
        Req->MElemSize, Copy->getDst(),
        Scheduler::getInstance().getDefaultHostQueue(), Req->MDims,
        Req->MAccessRange, Req->MAccessRange, /*DstOffset=*/{0, 0, 0},
        Req->MElemSize, std::move(RawEvents), Event);
    return CL_SUCCESS;
  }
  case CG::CGTYPE::COPY_PTR_TO_ACC: {
    CGCopy *Copy = (CGCopy *)MCommandGroup.get();
    Requirement *Req = (Requirement *)(Copy->getDst());
    AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);

    Scheduler::getInstance().getDefaultHostQueue();

    MemoryManager::copy(AllocaCmd->getSYCLMemObj(), Copy->getSrc(),
                        Scheduler::getInstance().getDefaultHostQueue(),
                        Req->MDims, Req->MAccessRange, Req->MAccessRange,
                        /*SrcOffset*/ {0, 0, 0}, Req->MElemSize,
                        AllocaCmd->getMemAllocation(), MQueue, Req->MDims,
                        Req->MMemoryRange, Req->MAccessRange, Req->MOffset,
                        Req->MElemSize, std::move(RawEvents), Event);

    return CL_SUCCESS;
  }
  case CG::CGTYPE::COPY_ACC_TO_ACC: {
    CGCopy *Copy = (CGCopy *)MCommandGroup.get();
    Requirement *ReqSrc = (Requirement *)(Copy->getSrc());
    Requirement *ReqDst = (Requirement *)(Copy->getDst());

    AllocaCommandBase *AllocaCmdSrc = getAllocaForReq(ReqSrc);
    AllocaCommandBase *AllocaCmdDst = getAllocaForReq(ReqDst);

    MemoryManager::copy(
        AllocaCmdSrc->getSYCLMemObj(), AllocaCmdSrc->getMemAllocation(), MQueue,
        ReqSrc->MDims, ReqSrc->MMemoryRange, ReqSrc->MAccessRange,
        ReqSrc->MOffset, ReqSrc->MElemSize, AllocaCmdDst->getMemAllocation(),
        MQueue, ReqDst->MDims, ReqDst->MMemoryRange, ReqDst->MAccessRange,
        ReqDst->MOffset, ReqDst->MElemSize, std::move(RawEvents), Event);
    return CL_SUCCESS;
  }
  case CG::CGTYPE::FILL: {
    CGFill *Fill = (CGFill *)MCommandGroup.get();
    Requirement *Req = (Requirement *)(Fill->getReqToFill());
    AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);

    MemoryManager::fill(
        AllocaCmd->getSYCLMemObj(), AllocaCmd->getMemAllocation(), MQueue,
        Fill->MPattern.size(), Fill->MPattern.data(), Req->MDims,
        Req->MMemoryRange, Req->MAccessRange, Req->MOffset, Req->MElemSize,
        std::move(RawEvents), Event);
    return CL_SUCCESS;
  }
  case CG::CGTYPE::RUN_ON_HOST_INTEL: {
    CGExecKernel *HostTask = (CGExecKernel *)MCommandGroup.get();

    // piEnqueueNativeKernel takes arguments blob which is passes to user
    // function.
    // Reserve extra space for the pointer to CGExecKernel to restore context.
    std::vector<void *> ArgsBlob(HostTask->MArgs.size() + 1);
    ArgsBlob[0] = (void *)HostTask;
    void **NextArg = ArgsBlob.data() + 1;

    if (MQueue->is_host()) {
      for (ArgDesc &Arg : HostTask->MArgs) {
        assert(Arg.MType == kernel_param_kind_t::kind_accessor);

        Requirement *Req = (Requirement *)(Arg.MPtr);
        AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);

        *NextArg = AllocaCmd->getMemAllocation();
        NextArg++;
      }

      if (!RawEvents.empty()) {
        // Assuming that the events are for devices to the same Plugin.
        const detail::plugin &Plugin = EventImpls[0]->getPlugin();
        Plugin.call<PiApiKind::piEventsWait>(RawEvents.size(), &RawEvents[0]);
      }
      DispatchNativeKernel((void *)ArgsBlob.data());
      return CL_SUCCESS;
    }

    std::vector<pi_mem> Buffers;
    // piEnqueueNativeKernel requires additional array of pointers to args blob,
    // values that pointers point to are replaced with actual pointers to the
    // memory before execution of user function.
    std::vector<void *> MemLocs;

    for (ArgDesc &Arg : HostTask->MArgs) {
      assert(Arg.MType == kernel_param_kind_t::kind_accessor);

      Requirement *Req = (Requirement *)(Arg.MPtr);
      AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);
      pi_mem MemArg = (pi_mem)AllocaCmd->getMemAllocation();

      Buffers.push_back(MemArg);
      MemLocs.push_back(NextArg);
      NextArg++;
    }
    const detail::plugin &Plugin = MQueue->getPlugin();
    pi_result Error = Plugin.call_nocheck<PiApiKind::piEnqueueNativeKernel>(
        MQueue->getHandleRef(), DispatchNativeKernel, (void *)ArgsBlob.data(),
        ArgsBlob.size() * sizeof(ArgsBlob[0]), Buffers.size(), Buffers.data(),
        const_cast<const void **>(MemLocs.data()), RawEvents.size(),
        RawEvents.empty() ? nullptr : RawEvents.data(), &Event);

    switch (Error) {
    case PI_INVALID_OPERATION:
      throw cl::sycl::runtime_error(
          "Device doesn't support run_on_host_intel tasks.", Error);
    case PI_SUCCESS:
      return Error;
    default:
      throw cl::sycl::runtime_error(
          "Enqueueing run_on_host_intel task has failed.", Error);
    }
  }
  case CG::CGTYPE::KERNEL: {
    CGExecKernel *ExecKernel = (CGExecKernel *)MCommandGroup.get();

    NDRDescT &NDRDesc = ExecKernel->MNDRDesc;

    if (MQueue->is_host()) {
      for (ArgDesc &Arg : ExecKernel->MArgs)
        if (kernel_param_kind_t::kind_accessor == Arg.MType) {
          Requirement *Req = (Requirement *)(Arg.MPtr);
          AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);
          Req->MData = AllocaCmd->getMemAllocation();
        }
      if (!RawEvents.empty()) {
        // Assuming that the events are for devices to the same Plugin.
        const detail::plugin &Plugin = EventImpls[0]->getPlugin();
        Plugin.call<PiApiKind::piEventsWait>(RawEvents.size(), &RawEvents[0]);
      }
      ExecKernel->MHostKernel->call(NDRDesc,
                                    getEvent()->getHostProfilingInfo());
      return CL_SUCCESS;
    }

    // Run OpenCL kernel
    sycl::context Context = MQueue->get_context();
    const detail::plugin &Plugin = MQueue->getPlugin();
    RT::PiKernel Kernel = nullptr;

    if (nullptr != ExecKernel->MSyclKernel) {
      assert(ExecKernel->MSyclKernel->get_info<info::kernel::context>() ==
             Context);
      Kernel = ExecKernel->MSyclKernel->getHandleRef();
    } else
      Kernel = detail::ProgramManager::getInstance().getOrCreateKernel(
          ExecKernel->MOSModuleHandle, Context, ExecKernel->MKernelName);

    for (ArgDesc &Arg : ExecKernel->MArgs) {
      switch (Arg.MType) {
      case kernel_param_kind_t::kind_accessor: {
        Requirement *Req = (Requirement *)(Arg.MPtr);
        AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);
#if USE_PI_CUDA
        pi_mem MemArg = (pi_mem)AllocaCmd->getMemAllocation();
        Plugin.call<PiApiKind::piextKernelSetArgMemObj>(Kernel, Arg.MIndex, &MemArg);
#else
        RT::PiMem MemArg = (RT::PiMem)AllocaCmd->getMemAllocation();
        Plugin.call<PiApiKind::piKernelSetArg>(Kernel, Arg.MIndex,
                                               sizeof(RT::PiMem), &MemArg);
        Plugin.call<PiApiKind::piKernelSetArg>(Kernel, Arg.MIndex,
                                               sizeof(RT::PiMem), &MemArg);
#endif
        break;
      }
      case kernel_param_kind_t::kind_std_layout: {
        Plugin.call<PiApiKind::piKernelSetArg>(Kernel, Arg.MIndex, Arg.MSize,
                                               Arg.MPtr);
        break;
      }
      case kernel_param_kind_t::kind_sampler: {
        sampler *SamplerPtr = (sampler *)Arg.MPtr;
        RT::PiSampler Sampler =
            detail::getSyclObjImpl(*SamplerPtr)->getOrCreateSampler(Context);
        Plugin.call<PiApiKind::piKernelSetArg>(Kernel, Arg.MIndex,
                                               sizeof(cl_sampler), &Sampler);
        break;
      }
      case kernel_param_kind_t::kind_pointer: {
        Plugin.call<PiApiKind::piextKernelSetArgPointer>(Kernel, Arg.MIndex,
                                                         Arg.MSize, Arg.MPtr);
        break;
      }
      default:
        assert(!"Unhandled");
      }
    }

    adjustNDRangePerKernel(NDRDesc, Kernel,
                           *(detail::getSyclObjImpl(MQueue->get_device())));

    // Some PI Plugins (like OpenCL) require this call to enable USM
    // For others, PI will turn this into a NOP.
    Plugin.call<PiApiKind::piKernelSetExecInfo>(Kernel, PI_USM_INDIRECT_ACCESS,
                                                sizeof(pi_bool), &PI_TRUE);

    // Remember this information before the range dimensions are reversed
    const bool HasLocalSize = (NDRDesc.LocalSize[0] != 0);

    ReverseRangeDimensionsForKernel(NDRDesc);

    pi_result Error = Plugin.call_nocheck<PiApiKind::piEnqueueKernelLaunch>(
        MQueue->getHandleRef(), Kernel, NDRDesc.Dims, &NDRDesc.GlobalOffset[0],
        &NDRDesc.GlobalSize[0], HasLocalSize ? &NDRDesc.LocalSize[0] : nullptr,
        RawEvents.size(), RawEvents.empty() ? nullptr : &RawEvents[0], &Event);

    if (PI_SUCCESS != Error) {
      // If we have got non-success error code, let's analyze it to emit nice
      // exception explaining what was wrong
      const device_impl &DeviceImpl =
          *(detail::getSyclObjImpl(MQueue->get_device()));
      return detail::enqueue_kernel_launch::handleError(Error, DeviceImpl,
                                                        Kernel, NDRDesc);
    }
    return PI_SUCCESS;
  }
  case CG::CGTYPE::COPY_USM: {
    CGCopyUSM *Copy = (CGCopyUSM *)MCommandGroup.get();
    MemoryManager::copy_usm(Copy->getSrc(), MQueue, Copy->getLength(),
                            Copy->getDst(), std::move(RawEvents), Event);
    return CL_SUCCESS;
  }
  case CG::CGTYPE::FILL_USM: {
    CGFillUSM *Fill = (CGFillUSM *)MCommandGroup.get();
    MemoryManager::fill_usm(Fill->getDst(), MQueue, Fill->getLength(),
                            Fill->getFill(), std::move(RawEvents), Event);
    return CL_SUCCESS;
  }
  case CG::CGTYPE::PREFETCH_USM: {
    CGPrefetchUSM *Prefetch = (CGPrefetchUSM *)MCommandGroup.get();
    MemoryManager::prefetch_usm(Prefetch->getDst(), MQueue,
                                Prefetch->getLength(), std::move(RawEvents),
                                Event);
    return CL_SUCCESS;
  }
  case CG::CGTYPE::INTEROP_TASK_CODEPLAY: {
    const detail::plugin &Plugin = MQueue->getPlugin();
    CGInteropTask *ExecInterop = (CGInteropTask *)MCommandGroup.get();
    // Wait for dependencies to complete before dispatching work on the host
    // TODO: Use a callback to dispatch the interop task instead of waiting for
    //  the event
    if (!RawEvents.empty()) {
      Plugin.call<PiApiKind::piEventsWait>(RawEvents.size(), &RawEvents[0]);
    }
    std::vector<interop_handler::ReqToMem> ReqMemObjs;
    // Extract the Mem Objects for all Requirements, to ensure they are available if
    // a user ask for them inside the interop task scope
    const auto& HandlerReq = ExecInterop->MRequirements;
    std::for_each(std::begin(HandlerReq), std::end(HandlerReq), [&](Requirement* Req) {
      AllocaCommandBase *AllocaCmd = getAllocaForReq(Req);
      auto MemArg = reinterpret_cast<pi_mem>(AllocaCmd->getMemAllocation());
      interop_handler::ReqToMem ReqToMem = std::make_pair(Req, MemArg);
      ReqMemObjs.emplace_back(ReqToMem);
    });

    auto interop_queue = MQueue->get();
    std::sort(std::begin(ReqMemObjs), std::end(ReqMemObjs));
    interop_handler InteropHandler(std::move(ReqMemObjs), interop_queue);
    ExecInterop->MInteropTask->call(InteropHandler);
    Plugin.call<PiApiKind::piEnqueueEventsWait>(MQueue->getHandleRef(), 0, nullptr, &Event);
    Plugin.call<PiApiKind::piQueueRelease>(reinterpret_cast<pi_queue>(interop_queue));
    return CL_SUCCESS;
  }
  case CG::CGTYPE::NONE:
  default:
    throw runtime_error("CG type not implemented.", PI_INVALID_OPERATION);
  }
}

} // namespace detail
} // namespace sycl
} // __SYCL_INLINE_NAMESPACE(cl)
