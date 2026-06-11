#ifndef OFE_CORE_EVENT_BUS_H
#define OFE_CORE_EVENT_BUS_H

#include <cstdint>

#include <ofe/signals/signal_types.h>

namespace ofe::core {

class EventBus {
 public:
  virtual ~EventBus() = default;

  virtual void publish(const ofe::signals::SignalEvent& event) = 0;
};

} // namespace ofe::core

#endif
