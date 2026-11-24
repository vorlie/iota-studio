# Iota Studio MCP Support (Opt-In)

Iota Studio includes an optional **Model Context Protocol (MCP)** server (`iota_mcp.exe`) and a local **Live Studio Bridge** inside `iota_studio.exe` so AI coding/creative agents can inspect, build, preview, and export visualizers.

By default, MCP is **disabled** (`IOTA_BUILD_MCP=OFF`) so standard desktop builds have zero extra code, runtime overhead, or UI items.

---

## Building With or Without MCP

### Standard Build (No MCP Bloat — Default)
```pwsh
cmake --preset windows-msvc-ninja-release
cmake --build --preset release --parallel
```
Only `iota_studio.exe` and the core libraries are built (`build/release/`).

### MCP-Enabled Build (`IOTA_BUILD_MCP=ON`)
```pwsh
cmake --preset windows-msvc-ninja-release-mcp
cmake --build --preset release-mcp --parallel
```
Outputs in `build/release-mcp/`:
- `apps/mcp/iota_mcp.exe` — Standalone stdio MCP server (`stdin`/`stdout` JSON-RPC 2.0)
- `apps/studio/iota_studio.exe` — Built with `IOTA_ENABLE_MCP=1` (includes the opt-in **MCP** menu)

---

## Operating Modes

1. **Standalone Headless Mode (Default):**
   Agents launch `iota_mcp.exe`, create or open projects, inspect properties/documentation, load audio or synthetic test tones, render OpenGL 4.5 PNG previews inline (`render_preview_frame`), and export MP4 videos (`export_video` with GPU hardware encoder support).

2. **Live Studio Bridge (Interactive GUI Mode):**
   - In the MCP-enabled `iota_studio.exe`, click **MCP -> Enable Live Studio Bridge**.
   - **Automatic Working-Copy Safety:** If an existing project (`.iota`, `.viz`, `.json`) is open, Studio immediately creates a sibling copy named **`<name>.mcp.iota`** (incrementing to `.mcp_2.iota` if needed) and switches the open session onto that copy before listening. Your original `.iota` / `.viz` file is never touched by agent edits or saves.
   - If you open a different file manually in Studio, Live Bridge automatically turns off so newly opened originals stay protected until you re-enable it.
   - Agents connect over the per-user local Windows Named Pipe (`\\.\pipe\iota_studio_mcp_<user>`) either by passing `--attach` on startup or calling the `studio_connect` tool, and can disconnect back to headless mode via `studio_disconnect`.
   - Every agent mutation pushes an Undo checkpoint onto Studio's `Ctrl+Z` / `Ctrl+Y` stack.

---

## MCP Client Configuration

Add `iota_mcp.exe` to your MCP client (e.g. Antigravity, Claude Desktop, or Cursor):

```json
{
  "mcpServers": {
    "iota-studio": {
      "command": "PATH-TO/iota_mcp.exe",
      "args": []
    }
  }
}
```

Pass `"args": ["--attach"]` if you want it to automatically attach to a running Studio Live Bridge on startup (it falls back cleanly to headless mode when Studio is closed).

---

## Exposed Tools & Resources

### Tools
- **Projects & Assets:** `project_new`, `project_open`, `project_save`, `project_get_summary`, `project_set_metadata`, `project_import_asset`
- **Compositions & Hierarchy:** `composition_add`, `composition_rename`, `composition_duplicate`, `composition_delete`, `layer_add`, `layer_remove`, `layer_rename`, `layer_duplicate`, `layer_move`, `layer_set_visible`, `layer_solo`, `layer_precompose`
- **Properties, Shaders & Keyframes:** `layer_get_properties`, `layer_set_properties`, `layer_set_keyframe`, `layer_clear_keyframes`
- **Documentation Discovery:** `docs_list_elements`, `docs_get_element_schema`, `docs_search`
- **Audio, Preview & Export:** `audio_load`, `render_preview_frame`, `export_list_encoders`, `export_video`
- **Studio Session Control:** `studio_status`, `studio_playback`, `studio_undo`, `studio_redo`, `studio_connect`, `studio_disconnect`

### Resources
- `iota://docs/elements`
- `iota://docs/mvar-modes`
- `iota://docs/text-tokens`
- `iota://docs/custom-shader`
- `iota://project/current`
