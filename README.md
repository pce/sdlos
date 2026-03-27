# sdlos

C++23 / SDL3 GPU framework for building native applications with a declarative scene graph, CSS-driven styling, and a Jade-like **retained-mode** UI description language.

**Not a browser. Not Electron. A render tree that runs directly on the GPU.**

> **Status:** Early development. API subject to change. Contributions welcome! 
 

## Quick Start

### First-time setup
```bash
./pre_cmake.sh                 # clone SDL3 + SDL_image/mixer/ttf into deps/
```

### Using the sdlos CLI (recommended)
```bash
# One-time setup
cd tooling && uv sync && cd ..

# Create a new app
uv run sdlos create myapp

# Build and run
uv run sdlos run myapp         # build + launch
uv run sdlos run myapp --watch # rebuild on source changes
```

### Using CMake directly
```bash
# Configure
cmake --preset macos-debug     # or macos-release, asan-system, etc.

# Build all targets
cmake --build build --parallel

# Build a specific app
cmake --build build --target styleguide
```

## Key Features

###  Retained-Mode UI
Define UI with [JadeLite](docs/summary.adoc#jadelite) — a clean, indentation-sensitive language similar to Pug:
```jade
row.toolbar
  button.key(onclick="calc:press" data-value="7") 7
  div.timer(interval-ms="1000" ontick="clock:tick")
  div.card(onreveal="anim:enter")
```

###  CSS-Driven Styling
Apply styles via CSS with support for `:hover`, `:active`, to any node in the render tree:
```css
button.key {
  background-color: #1e88e5;
  font-size: 18;
}

button.key:hover {
  background-color: #1565c0;
}
```

 

###  Single-Pass Layout
- O(n) preorder cascade: no fixpoint loops
- Flex-based layout (`FlexRow`, `FlexColumn`, `Block`)
- Press **F1** at runtime to visualize layout hierarchy


### GPU-Accelerated Rendering
- Fragment shaders on any node
- 3D scene support with `.glb` models and PBR materials
- Animated transitions via `Animated<T>`

### Zero Dependencies

Brings SDL, harfbuzz, and many more to the party

- Vendored SDL3, SDL_image, SDL_mixer, SDL_ttf
- Run `./pre_cmake.sh` once, then build offline


## Architecture



### Render Tree
LCRS (Left-Child Right-Sibling) topology for cache-efficient traversal:
```cpp
struct RenderNode {
    NodeHandle parent, child, sibling;  // stable SlotIDs
    float x, y, w, h;                   // physical pixels
    bool dirty_layout, dirty_render;
    std::function<void(RenderContext&)> draw;
    std::function<void()> update;
    std::any state;
};
```

```
Jade (declarative UI)
↓
RenderTree (retained-mode) 
↓
CSS styling (declarative)
↓
GPU shaders (functional evaluation, NO draw calls)
↓
Perfect output (works at any DPI)
```


### Frame Pipeline

```
1. Acquire command buffer & swapchain texture
2. Sync window size → mark layout dirty
3. TextRenderer::flushUploads()
4. Wallpaper shader (render pass)
5. 3D scene pre-pass (if attached)
6. 2D UI rendering
7. Submit GPU command buffer
```


### Key Components

| Component | Purpose |
|-----------|---------|
| **RenderTree** | LCRS node hierarchy, O(1) insert/erase/lookup |
| **StyleSheet** | CSS parsing & application (selectors: tag, `.class`, `#id`, `:hover`, `:active`) |
| **EventBus** | Publish-subscribe event routing |
| **Signal<T>** | Reactive value cells with observers |
| **Animated<T>** | Value interpolation with easing functions |
| **GltfScene** | 3D model loading & PBR rendering |

## Platform Support

| Platform | Backend | Shader Format |
|----------|---------|---------------|
| macOS / iOS | Metal | MSL |
| Linux | Vulkan | SPIR-V |
| Windows | Vulkan / D3D12 | SPIR-V / DXIL |

## Creating an App

### Using sdlos CLI (interactive)
```bash
uv run sdlos create myapp                                # fully interactive
uv run sdlos create myapp --data-dir                     # with data folder
uv run sdlos create myapp --with-model assets/model.glb  # with 3D model
```

### Using a template
```bash
uv run sdlos templates # list 
uv run sdlos create myshader --template shader --win-w 1280 --win-h 800
uv run sdlos create mycam --template camera --with-model model.glb
```

### App structure
After creation, your app directory contains:
- `myapp.jade` — UI scene definition
- `myapp.css` — Styles
- `myapp_behavior.cc` — Event handlers & animations
- `myapp.cmake` — CMake build rules
- `data/` — Runtime assets (images, models, audio)

## Building & Testing

### Build targets
```bash
cmake --build build --target styleguide     # UI showcase
cmake --build build --target calculator     # Simple calculator
cmake --build build --target pug            # FrameGraph / shader pipeline demo
cmake --build build --target test_jade      # Unit tests
```

### Run tests
```bash
cmake --build build --target test_jade
./build/test_jade --reporter compact

cd tooling && uv run pytest
```

### Code formatting
```bash
cmake --build build --target format        # auto-format all sources
cmake --build build --target format-check  # CI: check formatting
```

## Examples

### Text Box
```cpp
auto box = makeTextBox(tree, {.placeholder = "Name", .w = 280.f});
tree.appendChild(parent, box.root);
box.setText("Alice");
box.focus();
```

### Animated Transition
```cpp
Animated<float> opacity;
opacity.transition(1.f, 300.f, easing::easeOut);

// In update():
if (!opacity.finished())
    tree.markDirty(handle);

// In draw():
ctx.drawRect(ax, ay, w, h, r, g, b, opacity.current());
```

### 3D Scene with PBR
```jade
scene3d(src="data/models/altar.glb" mesh-id="altar")
```

```css
#altar {
    --translate-z: -1;
    --scale: 1;
    --roughness: 0.35;
    --metallic: 0.15;
    --emissive: #ffcc66;
}
#altar:hover {
    border-color: #ffd700;
    border-width: 3;
}
```

### Fragment Shader on Node
```cpp
struct NodeShaderParams {   // Fixed 32-byte uniform block
    float u_width, u_height;
    float u_focusX, u_focusY;
    float u_param0, u_param1, u_param2;
    float u_time;
};
```

Set `_shader="myeffect"` on any node to run a custom fragment shader. See `examples/apps/cam/` and `examples/apps/shade/` for complete Metal + GLSL + WGSL examples.




## Documentation

- **[Examples](examples/apps/)** — Complete working applications, in progress

## License

MIT

