#include "dream/core/module_registry.h"
#include "dream/debug_ui/debug_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

namespace dream::modules {
namespace {

using namespace debug_ui;

ImGuiKey to_imgui(platform::Key key) {
    using platform::Key;
    if (key >= Key::A && key <= Key::Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (static_cast<int>(key) - static_cast<int>(Key::A)));
    }
    if (key >= Key::Digit0 && key <= Key::Digit9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 +
                                     (static_cast<int>(key) - static_cast<int>(Key::Digit0)));
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 +
                                     (static_cast<int>(key) - static_cast<int>(Key::F1)));
    }
    switch (key) {
    case Key::Space:
        return ImGuiKey_Space;
    case Key::Enter:
        return ImGuiKey_Enter;
    case Key::Escape:
        return ImGuiKey_Escape;
    case Key::Tab:
        return ImGuiKey_Tab;
    case Key::Backspace:
        return ImGuiKey_Backspace;
    case Key::Delete:
        return ImGuiKey_Delete;
    case Key::Home:
        return ImGuiKey_Home;
    case Key::End:
        return ImGuiKey_End;
    case Key::Left:
        return ImGuiKey_LeftArrow;
    case Key::Right:
        return ImGuiKey_RightArrow;
    case Key::Up:
        return ImGuiKey_UpArrow;
    case Key::Down:
        return ImGuiKey_DownArrow;
    case Key::LeftShift:
        return ImGuiKey_LeftShift;
    case Key::RightShift:
        return ImGuiKey_RightShift;
    case Key::LeftControl:
        return ImGuiKey_LeftCtrl;
    case Key::RightControl:
        return ImGuiKey_RightCtrl;
    case Key::LeftAlt:
        return ImGuiKey_LeftAlt;
    case Key::RightAlt:
        return ImGuiKey_RightAlt;
    default:
        return ImGuiKey_None;
    }
}

// Labels from string_view need a terminating zero for ImGui.
class Label {
public:
    explicit Label(std::string_view text) : m_text(text) {}
    [[nodiscard]] const char* c_str() const noexcept {
        return m_text.c_str();
    }

private:
    std::string m_text;
};

class ImGuiDebugUi final : public DebugUi {
public:
    ImGuiDebugUi() {
        IMGUI_CHECKVERSION();
        m_context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendRendererName = "dream_engine";
        io.BackendPlatformName = "dream_engine";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::StyleColorsDark();
    }

    ~ImGuiDebugUi() override {
        ImGui::SetCurrentContext(m_context);
        // Releases the textures ImGui asked for, such as the font atlas.
        if (m_host != nullptr) {
            for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
                if (texture->TexID != ImTextureID_Invalid) {
                    m_host->release_texture(static_cast<ui::TextureId>(texture->TexID));
                    texture->SetTexID(ImTextureID_Invalid);
                    texture->SetStatus(ImTextureStatus_Destroyed);
                }
            }
        }
        ImGui::DestroyContext(m_context);
    }

    ImGuiDebugUi(const ImGuiDebugUi&) = delete;
    ImGuiDebugUi& operator=(const ImGuiDebugUi&) = delete;

    core::Result<void> initialize(ui::Host& host) override {
        m_host = &host;
        return {};
    }

    bool handle_event(const platform::Event& event) override {
        ImGui::SetCurrentContext(m_context);
        ImGuiIO& io = ImGui::GetIO();
        return std::visit(
            [&](const auto& value) -> bool {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, platform::PointerMoved>) {
                    io.AddMousePosEvent(value.x, value.y);
                    return io.WantCaptureMouse;
                } else if constexpr (std::is_same_v<Type, platform::PointerButtonChanged>) {
                    const int button = value.button == platform::PointerButton::Right    ? 1
                                       : value.button == platform::PointerButton::Middle ? 2
                                                                                         : 0;
                    io.AddMouseSourceEvent(value.button == platform::PointerButton::Touch
                                               ? ImGuiMouseSource_TouchScreen
                                               : ImGuiMouseSource_Mouse);
                    io.AddMouseButtonEvent(button, value.pressed);
                    return io.WantCaptureMouse;
                } else if constexpr (std::is_same_v<Type, platform::PointerWheel>) {
                    io.AddMouseWheelEvent(value.x, value.y);
                    return io.WantCaptureMouse;
                } else if constexpr (std::is_same_v<Type, platform::KeyChanged>) {
                    update_modifiers(value);
                    if (const ImGuiKey key = to_imgui(value.key); key != ImGuiKey_None) {
                        io.AddKeyEvent(key, value.pressed);
                    }
                    return io.WantCaptureKeyboard;
                } else if constexpr (std::is_same_v<Type, platform::TextEntered>) {
                    io.AddInputCharactersUTF8(value.text.c_str());
                    return io.WantTextInput;
                } else if constexpr (std::is_same_v<Type, platform::AppPaused>) {
                    io.AddFocusEvent(false);
                    return false;
                } else if constexpr (std::is_same_v<Type, platform::AppResumed>) {
                    io.AddFocusEvent(true);
                    return false;
                } else {
                    return false;
                }
            },
            event);
    }

    void begin_frame(const ui::FrameInfo& frame) override {
        ImGui::SetCurrentContext(m_context);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(frame.size.width),
                                static_cast<float>(frame.size.height));
        io.DeltaTime = std::max(frame.delta_seconds, 1.0f / 1000.0f);

        // Text and controls grow with the screen's density.
        const float scale = std::clamp(frame.display_scale, 0.5f, 6.0f);
        if (scale != m_scale) {
            ImGuiStyle& style = ImGui::GetStyle();
            style = ImGuiStyle();
            ImGui::StyleColorsDark(&style);
            style.ScaleAllSizes(scale);
            style.FontScaleDpi = scale;
            m_scale = scale;
        }
        ImGui::NewFrame();
    }

    bool begin_window(std::string_view title, bool* open) override {
        // Wide enough for a label beside each slider, but within a narrow phone screen, where new
        // windows open 60 pixels from the left. The height fits the contents.
        const float room = ImGui::GetIO().DisplaySize.x - 70.0f;
        ImGui::SetNextWindowSize(ImVec2(std::min(380.0f * m_scale, std::max(room, 200.0f)), 0.0f),
                                 ImGuiCond_FirstUseEver);
        return ImGui::Begin(Label(title).c_str(), open);
    }

    void end_window() override {
        ImGui::End();
    }

    void text(std::string_view text) override {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    }

    bool button(std::string_view label) override {
        return ImGui::Button(Label(label).c_str());
    }

    bool checkbox(std::string_view label, bool& value) override {
        return ImGui::Checkbox(Label(label).c_str(), &value);
    }

    bool slider(std::string_view label, float& value, float minimum, float maximum) override {
        return ImGui::SliderFloat(Label(label).c_str(), &value, minimum, maximum);
    }

    bool slider(std::string_view label, int& value, int minimum, int maximum) override {
        return ImGui::SliderInt(Label(label).c_str(), &value, minimum, maximum);
    }

    void plot(std::string_view label, std::span<const float> values, float minimum,
              float maximum) override {
        ImGui::PlotLines(Label(label).c_str(), values.data(), static_cast<int>(values.size()), 0,
                         nullptr, minimum, maximum, ImVec2(0.0f, 40.0f * m_scale));
    }

    void separator() override {
        ImGui::Separator();
    }

    void same_line() override {
        ImGui::SameLine();
    }

    const ui::DrawList& end_frame() override {
        ImGui::Render();
        const ImGuiIO& io = ImGui::GetIO();
        m_wants_pointer = io.WantCaptureMouse;
        m_wants_keyboard = io.WantCaptureKeyboard || io.WantTextInput;
        ImDrawData* const data = ImGui::GetDrawData();
        update_textures(*data);
        convert(*data);
        return m_list;
    }

    bool wants_pointer() const override {
        return m_wants_pointer;
    }

    bool wants_keyboard() const override {
        return m_wants_keyboard;
    }

private:
    void update_modifiers(const platform::KeyChanged& event) {
        using platform::Key;
        ImGuiIO& io = ImGui::GetIO();
        const auto track = [&](Key left, Key right, bool& left_down, bool& right_down,
                               ImGuiKey modifier) {
            if (event.key == left || event.key == right) {
                (event.key == left ? left_down : right_down) = event.pressed;
                io.AddKeyEvent(modifier, left_down || right_down);
            }
        };
        track(Key::LeftControl, Key::RightControl, m_control[0], m_control[1], ImGuiMod_Ctrl);
        track(Key::LeftShift, Key::RightShift, m_shift[0], m_shift[1], ImGuiMod_Shift);
        track(Key::LeftAlt, Key::RightAlt, m_alt[0], m_alt[1], ImGuiMod_Alt);
    }

    // Honors ImGui's requests to create, update and destroy textures, such as the font atlas,
    // which grows as new sizes and characters are needed.
    void update_textures(ImDrawData& data) {
        if (data.Textures == nullptr || m_host == nullptr) {
            return;
        }
        for (ImTextureData* texture : *data.Textures) {
            switch (texture->Status) {
            case ImTextureStatus_WantCreate:
            case ImTextureStatus_WantUpdates: {
                // Textures can't be changed in place, so an update replaces the whole texture.
                if (texture->TexID != ImTextureID_Invalid) {
                    m_host->release_texture(static_cast<ui::TextureId>(texture->TexID));
                }
                const auto width = static_cast<std::uint32_t>(texture->Width);
                const auto height = static_cast<std::uint32_t>(texture->Height);
                const std::span<const std::byte> pixels(
                    static_cast<const std::byte*>(texture->GetPixels()),
                    static_cast<std::size_t>(texture->GetSizeInBytes()));
                const core::Result<ui::TextureId> created =
                    texture->Format == ImTextureFormat_RGBA32
                        ? m_host->create_texture(width, height, pixels, ui::TextureKind::Coverage)
                        : core::Result<ui::TextureId>(core::Error{
                              core::ErrorCode::InvalidArgument, "Unexpected ImGui texture format"});
                texture->SetTexID(created.has_value() ? static_cast<ImTextureID>(created.value())
                                                      : ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_OK);
                break;
            }
            case ImTextureStatus_WantDestroy:
                if (texture->UnusedFrames > 0) {
                    if (texture->TexID != ImTextureID_Invalid) {
                        m_host->release_texture(static_cast<ui::TextureId>(texture->TexID));
                    }
                    texture->SetTexID(ImTextureID_Invalid);
                    texture->SetStatus(ImTextureStatus_Destroyed);
                }
                break;
            default:
                break;
            }
        }
    }

    // Merges ImGui's lists into one, with 32-bit indices.
    void convert(const ImDrawData& data) {
        m_list.clear();
        m_list.premultiplied_alpha = false;
        for (const ImDrawList* list : data.CmdLists) {
            const auto base = static_cast<std::uint32_t>(m_list.vertices.size());
            for (const ImDrawVert& vertex : list->VtxBuffer) {
                m_list.vertices.push_back(ui::Vertex{
                    .position = core::Vec2{vertex.pos.x, vertex.pos.y},
                    .uv = core::Vec2{vertex.uv.x, vertex.uv.y},
                    .color = vertex.col,
                });
            }
            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.UserCallback != nullptr || command.ElemCount == 0) {
                    continue;
                }
                const auto first = static_cast<std::uint32_t>(m_list.indices.size());
                for (unsigned int i = 0; i < command.ElemCount; ++i) {
                    m_list.indices.push_back(base + command.VtxOffset +
                                             list->IdxBuffer[static_cast<int>(command.IdxOffset + i)]);
                }
                const ImVec4 clip = command.ClipRect;
                m_list.commands.push_back(ui::DrawCommand{
                    .first_index = first,
                    .index_count = command.ElemCount,
                    .texture = static_cast<ui::TextureId>(command.GetTexID()),
                    .clip =
                        ui::ClipRect{
                            .x = static_cast<std::int32_t>(clip.x),
                            .y = static_cast<std::int32_t>(clip.y),
                            .width = static_cast<std::int32_t>(clip.z - clip.x),
                            .height = static_cast<std::int32_t>(clip.w - clip.y),
                        },
                });
            }
        }
    }

    ImGuiContext* m_context = nullptr;
    ui::Host* m_host = nullptr;
    ui::DrawList m_list;
    float m_scale = 0.0f;
    bool m_wants_pointer = false;
    bool m_wants_keyboard = false;
    bool m_control[2] = {false, false};
    bool m_shift[2] = {false, false};
    bool m_alt[2] = {false, false};
};

core::Result<std::unique_ptr<DebugUi>> create(const core::Config& /*config*/) {
    return std::unique_ptr<DebugUi>{std::make_unique<ImGuiDebugUi>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_debug_ui_imgui(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<DebugUi>{.name = "imgui", .create = &create});
}

} // namespace dream::modules
