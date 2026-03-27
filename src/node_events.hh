#pragma once

// node_events — RenderTree × EventBus wiring.
// Works on any RenderTree (JadeLite, XML, raw C++).
//
// Event attributes (set in node.styles by any parser):
//   onclick="topic"         — published on pointer click hit-test
//   data-value="payload"    — forwarded as EventBus payload with onclick / onselect
//   onreveal="topic"        — published once on the node's first update()
//   interval-ms="n"         — timer period; requires ontick
//   ontick="topic"          — published every interval-ms via update()
//   onselect="topic"        — published by dispatchSelect()
//
// onclick and onselect require no pre-registration — read on demand at dispatch time.
// onreveal and interval-ms+ontick require bindNodeEvents() to install update() callbacks.
//
// bindNodeEvents composes new callbacks AFTER any existing update() on each node.
// bus must outlive the RenderTree (callbacks hold a raw IEventBus& reference).

#include "render_tree.hh"
#include "hit_test.hh"
#include "i_event_bus.hh"

namespace pce::sdlos {

// hitTest → read onclick style → bus.publish(topic, data-value).
// No-op publish when the hit node has no onclick style.
[[nodiscard]]
NodeHandle dispatchClick(RenderTree& tree, NodeHandle root,
                         float px, float py, IEventBus& bus);

// bus.publish(onselect, data-value) on the given node. No-op when onselect is absent.
void dispatchSelect(RenderTree& tree, NodeHandle handle, IEventBus& bus);

// Walk the subtree and install update() callbacks for interval-ms+ontick and onreveal nodes.
void bindNodeEvents(RenderTree& tree, NodeHandle root, IEventBus& bus);

} // namespace pce::sdlos
