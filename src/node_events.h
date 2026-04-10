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

#include "hit_test.h"
#include "i_event_bus.h"
#include "render_tree.h"

namespace pce::sdlos {

namespace css {
struct StyleSheet;
}  // namespace css

// hitTest → read onclick style → bus.publish(topic, data-value).
// When css is non-null and the hit node (or an ancestor) is a child of a
// toggle-group parent, activates it radio-style before publishing the event.
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
/**
 * @brief Dispatches click
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param px    Horizontal coordinate in logical pixels
 * @param py    Vertical coordinate in logical pixels
 * @param bus   Blue channel component [0, 1]
 * @param css   css::StyleSheet * value
 *
 * @return Handle to the node, or k_null_handle on failure
 *
 * @warning Parameter 'css' is a non-const raw pointer — Raw pointer parameter —
 *          ownership is ambiguous; consider std::span (non-owning view),
 *          std::unique_ptr (transfer), or const T* (borrow)
 */
[[nodiscard]]
NodeHandle dispatchClick(
    RenderTree &tree,
    NodeHandle root,
    float px,
    float py,
    IEventBus &bus,
    css::StyleSheet *css = nullptr);

// bus.publish(onselect, data-value) on the given node. No-op when onselect is absent.
/**
 * @brief Dispatches select
 *
 * @param tree    Red channel component [0, 1]
 * @param handle  Opaque resource handle
 * @param bus     Blue channel component [0, 1]
 */
void dispatchSelect(RenderTree &tree, NodeHandle handle, IEventBus &bus);

// Walk the subtree and install update() callbacks for interval-ms+ontick and onreveal nodes.
/**
 * @brief Binds node events
 *
 * @param tree  Red channel component [0, 1]
 * @param root  Red channel component [0, 1]
 * @param bus   Blue channel component [0, 1]
 */
void bindNodeEvents(RenderTree &tree, NodeHandle root, IEventBus &bus);

}  // namespace pce::sdlos
