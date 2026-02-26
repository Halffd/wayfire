# Wayfire Plugin Enhancements

This document describes the enhancements made to Wayfire plugins, including new IPC methods, configuration options, and functionality changes.

## Table of Contents

1. [Switcher Plugin](#switcher-plugin)
2. [Scale Plugin](#scale-plugin)
3. [Zoom Plugin](#zoom-plugin)
4. [WM-Actions Plugin](#wm-actions-plugin)
5. [Display Plugin](#display-plugin)

---

## Switcher Plugin

### Overview
The switcher plugin has been redesigned from a 3D carousel to a **thumbnail grid layout** that displays all windows from the current workspace.

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `view_thumbnail_width` | int | 600 | Width of each thumbnail in pixels. Height is calculated automatically to maintain aspect ratio. |
| `grid_width_percent` | int | 90 | Total grid width as percentage of screen width (e.g., 90 for 90% of screen). |
| `speed` | animation | 500ms | Duration of the animation in milliseconds. |

### Behavior
- Thumbnails are arranged in a grid with automatic rows/columns based on the number of windows
- Grid is centered both horizontally and vertically on the screen
- Navigation cycles through windows using Alt+Tab / Alt+Shift+Tab
- Selected window is brought to front

### Example Configuration
```ini
[switcher]
view_thumbnail_width = 600
grid_width_percent = 90
speed = 500
next_view = <alt> KEY_TAB
prev_view = <alt> <shift> KEY_TAB
```

---

## Scale Plugin

### Overview
The scale plugin now supports **hot corner triggering**, allowing you to activate the scale view by moving the cursor to screen corners.

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `corner_top_left` | string | none | Action for top-left corner. Values: `none`, `scale`, `scale_all` |
| `corner_top_right` | string | none | Action for top-right corner. Values: `none`, `scale`, `scale_all` |
| `corner_bottom_left` | string | none | Action for bottom-left corner. Values: `none`, `scale`, `scale_all` |
| `corner_bottom_right` | string | scale | Action for bottom-right corner. Values: `none`, `scale`, `scale_all` |
| `corner_zone_size` | int | 20 | Size in pixels of the hot corner zones (range: 1-200). |
| `corner_delay` | int | 250 | Delay in milliseconds before corner action triggers (range: 0-2000). |

### Corner Actions
- `none` - No action
- `scale` - Toggle scale for current workspace
- `scale_all` - Toggle scale showing windows from all workspaces

### Example Configuration
```ini
[scale]
# Hot corners
corner_bottom_right = scale
corner_top_left = scale_all
corner_zone_size = 20
corner_delay = 250

# Existing options
toggle = <super> KEY_P
duration = 750ms
spacing = 50
```

### Usage Tips
- The delay prevents accidental triggers when quickly moving through corners
- Corner tracking resets when scale activates or when the cursor leaves the corner zone
- Works with both mouse and touch input

---

## Zoom Plugin

### Overview
The zoom plugin now supports **keyboard shortcuts** and **IPC methods** for programmatic control.

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `modifier` | key | \<super\> | Modifier for mouse wheel zoom |
| `zoom_in` | key | \<ctrl\> KEY_UP | Keyboard shortcut to zoom in |
| `zoom_out` | key | \<ctrl\> KEY_DOWN | Keyboard shortcut to zoom out |
| `zoom_reset` | key | \<ctrl\> KEY_SLASH | Keyboard shortcut to reset zoom to 100% |
| `speed` | double | 0.01 | Speed factor for zooming |
| `smoothing_duration` | animation | 300ms | Duration of zoom transitions |
| `interpolation_method` | int | 0 | 0=Linear, 1=Nearest neighbor |

### IPC Methods

#### `zoom/setZoom`
Sets the zoom factor to a specific value.

**Parameters:**
- `factor` (double, required): Zoom factor from 1.0 to 50.0
- `animation` (bool, optional): Whether to animate the transition (default: true)

**Example:**
```json
{"method": "zoom/setZoom", "data": {"factor": 2.5, "animation": true}}
```

#### `zoom/zoomIn`
Increases the zoom level.

**Parameters:**
- `delta` (double, optional): Amount to zoom in (default: 1.0)

**Example:**
```json
{"method": "zoom/zoomIn", "data": {"delta": 0.5}}
```

#### `zoom/zoomOut`
Decreases the zoom level.

**Parameters:**
- `delta` (double, optional): Amount to zoom out (default: 1.0)

**Example:**
```json
{"method": "zoom/zoomOut", "data": {"delta": 0.5}}
```

#### `zoom/getZoom`
Returns the current zoom state.

**Returns:**
- `factor` (double): Current zoom factor
- `target` (double): Target zoom factor
- `is_animating` (bool): Whether animation is in progress
- `animation_duration` (int): Animation duration in milliseconds
- `interpolation_method` (int): Interpolation method (0=linear, 1=nearest)

**Example:**
```json
{"method": "zoom/getZoom"}
```

**Response:**
```json
{
  "factor": 2.5,
  "target": 2.5,
  "is_animating": false,
  "animation_duration": 300,
  "interpolation_method": 0
}
```

### Example Configuration
```ini
[zoom]
modifier = <super>
zoom_in = <ctrl> KEY_UP
zoom_out = <ctrl> KEY_DOWN
zoom_reset = <ctrl> KEY_SLASH
speed = 0.01
smoothing_duration = 300ms linear
interpolation_method = 0
```

---

## WM-Actions Plugin

### Overview
The wm-actions plugin now provides comprehensive **window management** and **mouse control** IPC methods.

### Window Management IPC Methods

#### `wm-actions/close`
Closes the specified view.

**Parameters:**
- `view-id` (uint, required): ID of the view to close

**Example:**
```json
{"method": "wm-actions/close", "data": {"view-id": 42}}
```

#### `wm-actions/move`
Moves a view to the specified position.

**Parameters:**
- `view-id` (uint, required): ID of the view to move
- `x` (int, required): Target X coordinate
- `y` (int, required): Target Y coordinate
- `animation` (bool, optional): Whether to animate the move (default: true)

**Example:**
```json
{"method": "wm-actions/move", "data": {"view-id": 42, "x": 100, "y": 200, "animation": true}}
```

#### `wm-actions/resize`
Resizes a view to the specified dimensions.

**Parameters:**
- `view-id` (uint, required): ID of the view to resize
- `width` (int, required): Target width
- `height` (int, required): Target height
- `animation` (bool, optional): Whether to animate the resize (default: true)

**Example:**
```json
{"method": "wm-actions/resize", "data": {"view-id": 42, "width": 800, "height": 600}}
```

#### `wm-actions/get-geometry`
Returns the geometry and state of a view.

**Parameters:**
- `view-id` (uint, required): ID of the view

**Returns:**
- `x`, `y` (int): Position coordinates
- `width`, `height` (int): Dimensions
- `workspace_x`, `workspace_y` (int): Workspace indices
- `minimized` (bool): Whether minimized
- `fullscreen` (bool): Whether fullscreen
- `activated` (bool): Whether activated

**Example:**
```json
{"method": "wm-actions/get-geometry", "data": {"view-id": 42}}
```

**Response:**
```json
{
  "x": 100,
  "y": 200,
  "width": 800,
  "height": 600,
  "workspace_x": 0,
  "workspace_y": 0,
  "minimized": false,
  "fullscreen": false,
  "activated": true
}
```

#### `wm-actions/set-geometry`
Sets the exact geometry of a view.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `x` (int, required): X coordinate
- `y` (int, required): Y coordinate
- `width` (int, required): Width
- `height` (int, required): Height

**Example:**
```json
{"method": "wm-actions/set-geometry", "data": {"view-id": 42, "x": 0, "y": 0, "width": 1920, "height": 1080}}
```

#### `wm-actions/focus`
Focuses and raises the specified view.

**Parameters:**
- `view-id` (uint, required): ID of the view to focus

**Example:**
```json
{"method": "wm-actions/focus", "data": {"view-id": 42}}
```

#### `wm-actions/maximize`
Toggles the maximized state of a view.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool, required): True to maximize, false to unmaximize

**Example:**
```json
{"method": "wm-actions/maximize", "data": {"view-id": 42, "state": true}}
```

#### `wm-actions/set-minimized`
Toggles the minimized state of a view.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool, required): True to minimize, false to restore

**Example:**
```json
{"method": "wm-actions/set-minimized", "data": {"view-id": 42, "state": true}}
```

#### `wm-actions/set-always-on-top`
Sets the always-on-top state of a view.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool, required): Whether to set always-on-top

**Example:**
```json
{"method": "wm-actions/set-always-on-top", "data": {"view-id": 42, "state": true}}
```

#### `wm-actions/set-fullscreen`
Sets the fullscreen state of a view.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool, required): Whether to set fullscreen

**Example:**
```json
{"method": "wm-actions/set-fullscreen", "data": {"view-id": 42, "state": true}}
```

#### `wm-actions/set-sticky`
Sets the sticky state of a view (visible on all workspaces).

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool, required): Whether to set sticky

**Example:**
```json
{"method": "wm-actions/set-sticky", "data": {"view-id": 42, "state": true}}
```

#### `wm-actions/send-to-back`
Sends the view behind all other views.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool): Ignored

**Example:**
```json
{"method": "wm-actions/send-to-back", "data": {"view-id": 42}}
```

#### `wm-actions/bring-to-front`
Brings the view to the front of the stack.

**Parameters:**
- `view-id` (uint, required): ID of the view
- `state` (bool): Ignored

**Example:**
```json
{"method": "wm-actions/bring-to-front", "data": {"view-id": 42}}
```

### Mouse Control IPC Methods

#### `input/get-cursor-position`
Returns the current cursor position in global coordinates.

**Returns:**
- `x` (double): X coordinate
- `y` (double): Y coordinate

**Example:**
```json
{"method": "input/get-cursor-position"}
```

**Response:**
```json
{
  "x": 960.5,
  "y": 540.25
}
```

#### `input/warp-cursor`
Moves the cursor to the specified position.

**Parameters:**
- `x` (double, required): Target X coordinate
- `y` (double, required): Target Y coordinate

**Example:**
```json
{"method": "input/warp-cursor", "data": {"x": 960, "y": 540}}
```

### Usage Examples

#### Python Example - Get all windows and focus one:
```python
import json
import socket

def call_ipc(method, data=None):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect("/tmp/wayfire-ipc.sock")
    request = json.dumps({"method": method, "data": data or {}})
    sock.send(request.encode())
    response = sock.recv(4096)
    sock.close()
    return json.loads(response)

# Get current window geometry
result = call_ipc("wm-actions/get-geometry", {"view-id": 42})
print(f"Window at: {result['x']}, {result['y']}")

# Focus a window
call_ipc("wm-actions/focus", {"view-id": 42})

# Move cursor to center of screen
call_ipc("input/warp-cursor", {"x": 960, "y": 540})
```

#### Bash Example - Using wayfire-ipc client:
```bash
# Zoom in
wayfire-ipc --method zoom/zoomIn --data '{"delta": 0.5}'

# Get cursor position
wayfire-ipc --method input/get-cursor-position

# Move window
wayfire-ipc --method wm-actions/move --data '{"view-id": 42, "x": 100, "y": 200}'

# Close window
wayfire-ipc --method wm-actions/close --data '{"view-id": 42}'
```

---

## Display Plugin

### Overview
The display plugin provides **per-monitor brightness, gamma, and color temperature adjustment** with keyboard shortcuts and IPC control. Uses GPU-accelerated shaders for smooth, real-time adjustments.

### Configuration Options

#### Default Values
| Option | Type | Default | Range | Description |
|--------|------|---------|-------|-------------|
| `brightness` | double | 1.0 | 0.1 - 2.0 | Default brightness level |
| `gamma` | double | 1.0 | 0.1 - 3.0 | Default gamma correction |
| `temperature` | int | 6500 | 1000 - 20000 | Default color temperature in Kelvin |

#### Keyboard Shortcuts
| Option | Default | Action |
|--------|---------|--------|
| `brightness_up` | \<ctrl\> \<super\> KEY_B | Increase brightness by 0.1 |
| `brightness_down` | \<ctrl\> \<super\> \<shift\> KEY_B | Decrease brightness by 0.1 |
| `gamma_up` | \<ctrl\> \<super\> KEY_G | Increase gamma by 0.1 |
| `gamma_down` | \<ctrl\> \<super\> \<shift\> KEY_G | Decrease gamma by 0.1 |
| `temperature_up` | \<ctrl\> \<super\> KEY_T | Increase temperature by 500K (cooler) |
| `temperature_down` | \<ctrl\> \<super\> \<shift\> KEY_T | Decrease temperature by 500K (warmer) |
| `reset_all` | \<ctrl\> \<super\> KEY_R | Reset all adjustments to defaults |

#### Animation
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `animation_duration` | animation | 300ms | Duration for smooth transitions |

### IPC Methods

#### `display/set-brightness`
Sets the brightness level for the output.

**Parameters:**
- `brightness` (double, required): Brightness value (0.1 to 2.0)
- `animation` (bool, optional): Whether to animate the transition (default: true)

**Example:**
```json
{"method": "display/set-brightness", "data": {"brightness": 1.2, "animation": true}}
```

#### `display/set-gamma`
Sets the gamma correction for the output.

**Parameters:**
- `gamma` (double, required): Gamma value (0.1 to 3.0)
- `animation` (bool, optional): Whether to animate the transition (default: true)

**Example:**
```json
{"method": "display/set-gamma", "data": {"gamma": 1.2}}
```

#### `display/set-temperature`
Sets the color temperature for the output in Kelvin. Lower values are warmer (more red), higher values are cooler (more blue).

**Parameters:**
- `temperature` (int, required): Temperature in Kelvin (1000 to 20000)
- `animation` (bool, optional): Whether to animate the transition (default: true)

**Example:**
```json
{"method": "display/set-temperature", "data": {"temperature": 5500, "animation": true}}
```

#### `display/get-state`
Returns the current display adjustment state.

**Returns:**
- `brightness` (double): Current brightness
- `gamma` (double): Current gamma
- `temperature` (int): Current temperature in Kelvin
- `output_name` (string): Output name
- `output_id` (uint): Output ID

**Example:**
```json
{"method": "display/get-state"}
```

**Response:**
```json
{
  "brightness": 1.2,
  "gamma": 1.1,
  "temperature": 5500,
  "output_name": "DP-1",
  "output_id": 93827465
}
```

#### `display/reset`
Resets all display adjustments to default values.

**Example:**
```json
{"method": "display/reset"}
```

### Color Temperature Guide

| Temperature (K) | Description | Use Case |
|-----------------|-------------|----------|
| 1000-3000 | Very Warm | Evening/night, reduces blue light |
| 3000-4500 | Warm | Relaxing, evening use |
| 4500-5500 | Neutral | General use, daylight simulation |
| 5500-6500 | Cool | Daylight, focus work |
| 6500+ | Very Cool | High focus, may cause eye strain |

### Example Configuration
```ini
[display]
# Default values
brightness = 1.0
gamma = 1.0
temperature = 6500

# Keyboard shortcuts
brightness_up = <ctrl> <super> KEY_B
brightness_down = <ctrl> <super> <shift> KEY_B
gamma_up = <ctrl> <super> KEY_G
gamma_down = <ctrl> <super> <shift> KEY_G
temperature_up = <ctrl> <super> KEY_T
temperature_down = <ctrl> <super> <shift> KEY_T
reset_all = <ctrl> <super> KEY_R

# Animation
animation_duration = 300ms
```

### Usage Examples

#### Python - Night Light Mode:
```python
import json
import socket
from datetime import datetime

def call_ipc(method, data=None):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect("/tmp/wayfire-ipc.sock")
    request = json.dumps({"method": method, "data": data or {}})
    sock.send(request.encode())
    response = sock.recv(4096)
    sock.close()
    return json.loads(response)

# Enable night mode (warmer temperature in evening)
hour = datetime.now().hour
if hour >= 20 or hour < 7:  # 8 PM to 7 AM
    call_ipc("display/set-temperature", {"temperature": 3500})
    call_ipc("display/set-brightness", {"brightness": 0.8})
else:
    call_ipc("display/set-temperature", {"temperature": 6500})
    call_ipc("display/set-brightness", {"brightness": 1.0})

# Get current state
state = call_ipc("display/get-state")
print(f"Brightness: {state['brightness']}, Temperature: {state['temperature']}K")
```

#### Bash - Quick Adjustments:
```bash
# Increase brightness
wayfire-ipc --method display/set-brightness --data '{"brightness": 1.3}'

# Set warmer temperature for evening
wayfire-ipc --method display/set-temperature --data '{"temperature": 4000}'

# Reset all adjustments
wayfire-ipc --method display/reset

# Check current settings
wayfire-ipc --method display/get-state
```

### Technical Notes
- Requires GLES2 support (not available with Vulkan or Pixman renderer)
- Adjustments are applied per-output (per-monitor)
- Uses Tanner Helland's algorithm for color temperature calculation
- All adjustments are GPU-accelerated for minimal performance impact
- Smooth animations prevent jarring transitions

---

## Building and Installation

To build the enhanced plugins:

```bash
cd /path/to/wayfire
meson setup builddir --prefix=/usr
ninja -C builddir
sudo ninja -C builddir install
```

Individual plugins can be built with:
```bash
ninja -C builddir plugins/single_plugins/libswitcher.so
ninja -C builddir plugins/scale/libscale.so
ninja -C builddir plugins/single_plugins/libzoom.so
ninja -C builddir plugins/wm-actions/libwm-actions.so
```

---

## Compatibility Notes

- All changes are backward compatible with existing configurations
- New configuration options have sensible defaults
- IPC methods require the IPC plugin to be enabled
- View IDs can be obtained through IPC or by monitoring Wayfire events
