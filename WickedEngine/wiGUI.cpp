#include "wiGUI.h"
#include "wiInput.h"
#include "wiPrimitive.h"
#include "wiProfiler.h"
#include "wiRenderer.h"
#include "wiTimer.h"
#include "wiEventHandler.h"
#include "wiFont.h"
#include "wiImage.h"
#include "wiTextureHelper.h"
#include "wiBacklog.h"
#include "wiHelper.h"

#include <sstream>

using namespace wi::graphics;
using namespace wi::primitive;

namespace wi::gui
{
	struct InternalState
	{
		wi::graphics::PipelineState PSO_colored;

		InternalState()
		{
			wi::Timer timer;

			static wi::eventhandler::Handle handle = wi::eventhandler::Subscribe(wi::eventhandler::EVENT_RELOAD_SHADERS, [this](uint64_t userdata) { LoadShaders(); });
			LoadShaders();

			wilog("wi::gui Initialized (%d ms)", (int)std::round(timer.elapsed()));
		}

		void LoadShaders()
		{
			PipelineStateDesc desc;
			desc.vs = wi::renderer::GetShader(wi::enums::VSTYPE_VERTEXCOLOR);
			desc.ps = wi::renderer::GetShader(wi::enums::PSTYPE_VERTEXCOLOR);
			desc.il = wi::renderer::GetInputLayout(wi::enums::ILTYPE_VERTEXCOLOR);
			desc.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHDISABLED);
			desc.bs = wi::renderer::GetBlendState(wi::enums::BSTYPE_TRANSPARENT);
			desc.rs = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
			desc.pt = PrimitiveTopology::TRIANGLESTRIP;
			wi::graphics::GetDevice()->CreatePipelineState(&desc, &PSO_colored);
		}
	};
	inline InternalState& gui_internal()
	{
		static InternalState internal_state;
		return internal_state;
	}

	// This is used so that elements that support scroll could disable other scrolling elements:
	//	As opposed to click and other interaction types, we don't want to disable scroll on every focused widget
	//	because that would block scrolling the parent if a child element is hovered
	static bool scroll_allowed = true;
	static bool typing_active = false;

	void GUI::Update(const wi::Canvas& canvas, float dt)
	{
		if (!visible || wi::backlog::isActive())
		{
			return;
		}

		auto range = wi::profiler::BeginRangeCPU("GUI Update");

		scroll_allowed = true;

		XMFLOAT4 pointer = wi::input::GetPointer();
		Hitbox2D pointerHitbox = Hitbox2D(XMFLOAT2(pointer.x, pointer.y), XMFLOAT2(1, 1));

		uint32_t priority = 0;

		focus = false;
		bool force_disable = false;
		for (size_t i = 0; i < widgets.size(); ++i)
		{
			Widget* widget = widgets[i]; // re index in loop, because widgets can be realloced while updating!
			widget->force_disable = force_disable;
			widget->Update(canvas, dt);
			widget->force_disable = false;

			if (widget->priority_change)
			{
				widget->priority_change = false;
				widget->priority = priority++;
			}
			else
			{
				widget->priority = ~0u;
			}

			const bool visible = widget->IsVisible();
			const WIDGETSTATE state = widget->GetState();

			if (visible && widget->hitBox.intersects(pointerHitbox))
			{
				focus = true;
			}
			if (visible && state > IDLE)
			{
				focus = true;
			}
			if (visible && state > FOCUS)
			{
				force_disable = true;
			}
		}

		if (priority > 0)
		{
			// Sort only if there are priority changes
			//	Use std::stable_sort instead of std::sort to preserve UI element order with equal priorities
			std::stable_sort(widgets.begin(), widgets.end(), [](const Widget* a, const Widget* b) {
				return a->priority < b->priority;
			});
		}

		wi::profiler::EndRange(range);
	}
	void GUI::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		if (!visible || widgets.empty())
		{
			return;
		}

		auto range_cpu = wi::profiler::BeginRangeCPU("GUI Render");
		auto range_gpu = wi::profiler::BeginRangeGPU("GUI Render", cmd);

		Rect scissorRect;
		scissorRect.bottom = (int32_t)(canvas.GetPhysicalHeight());
		scissorRect.left = (int32_t)(0);
		scissorRect.right = (int32_t)(canvas.GetPhysicalWidth());
		scissorRect.top = (int32_t)(0);

		GraphicsDevice* device = wi::graphics::GetDevice();

		device->EventBegin("GUI", cmd);
		// Rendering is back to front:
		for (size_t i = 0; i < widgets.size(); ++i)
		{
			const Widget* widget = widgets[widgets.size() - i - 1];
			device->BindScissorRects(1, &scissorRect, cmd);
			widget->Render(canvas, cmd);
		}

		device->BindScissorRects(1, &scissorRect, cmd);
		for (auto& x : widgets)
		{
			x->RenderTooltip(canvas, cmd);
		}

		device->EventEnd(cmd);

		wi::profiler::EndRange(range_cpu);
		wi::profiler::EndRange(range_gpu);
	}
	void GUI::AddWidget(Widget* widget)
	{
		if (widget != nullptr)
		{
			assert(std::find(widgets.begin(), widgets.end(), widget) == widgets.end()); // don't attach one widget twice!
			widgets.push_back(widget);
		}
	}
	void GUI::RemoveWidget(Widget* widget)
	{
		for (auto& x : widgets)
		{
			if (x == widget)
			{
				x = widgets.back();
				widgets.pop_back();
				break;
			}
		}
	}
	Widget* GUI::GetWidget(const std::string& name)
	{
		for (auto& x : widgets)
		{
			if (x->GetName() == name)
			{
				return x;
			}
		}
		return nullptr;
	}
	bool GUI::HasFocus() const
	{
		if (!visible)
		{
			return false;
		}
		if (IsTyping())
		{
			return true;
		}

		return focus;
	}
	bool GUI::IsTyping() const
	{
		if (!visible)
		{
			return false;
		}

		return typing_active;
	}
	void GUI::SetColor(wi::Color color, int id)
	{
		for (auto& widget : widgets)
		{
			widget->SetColor(color, id);
		}
	}
	void GUI::SetImage(wi::Resource resource, int id)
	{
		for (auto& widget : widgets)
		{
			widget->SetImage(resource, id);
		}
	}
	void GUI::SetShadowColor(wi::Color color)
	{
		for (auto& widget : widgets)
		{
			widget->SetShadowColor(color);
		}
	}
	void GUI::SetTheme(const Theme& theme, int id)
	{
		for (auto& widget : widgets)
		{
			widget->SetTheme(theme, id);
		}
	}
	void GUI::ExportLocalization(wi::Localization& localization) const
	{
		wi::Localization& section = localization.GetSection("gui");
		for (auto& widget : widgets)
		{
			widget->ExportLocalization(section);
		}
	}
	void GUI::ImportLocalization(const wi::Localization& localization)
	{
		const wi::Localization* section = localization.CheckSection("gui");
		if (section == nullptr)
			return;
		for (auto& widget : widgets)
		{
			widget->ImportLocalization(*section);
		}
	}



	Widget::Widget()
	{
		sprites[IDLE].params.color = wi::Color::Booger();
		sprites[FOCUS].params.color = wi::Color::Gray();
		sprites[ACTIVE].params.color = wi::Color::White();
		sprites[DEACTIVATING].params.color = wi::Color::Gray();
		font.params.shadowColor = wi::Color::Shadow();
		font.params.shadow_bolden = 0.2f;
		font.params.shadow_softness = 0.2f;

		tooltipSprite.params.color = wi::Color(255, 234, 165);
		tooltipFont.params.color = wi::Color(25, 25, 25, 255);

		for (int i = IDLE; i < WIDGETSTATE_COUNT; ++i)
		{
			sprites[i].params.blendFlag = wi::enums::BLENDMODE_OPAQUE;
			sprites[i].params.enableBackground();
		}

		active_area.pos.x = 0;
		active_area.pos.y = 0;
		active_area.siz.x = std::numeric_limits<float>::max();
		active_area.siz.y = std::numeric_limits<float>::max();
	}
	void Widget::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		if (force_disable)
		{
			state = IDLE;
		}

		UpdateTransform();

		if (parent != nullptr)
		{
			this->UpdateTransform_Parented(*parent);
		}

		XMVECTOR S, R, T;
		XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&world));
		XMStoreFloat3(&translation, T);
		XMStoreFloat3(&scale, S);

		scale = wi::math::Max(scale, XMFLOAT3(0.001f, 0.001f, 0.001f));

		scissorRect.bottom = (int32_t)std::ceil(translation.y + scale.y);
		scissorRect.left = (int32_t)std::floor(translation.x);
		scissorRect.right = (int32_t)std::ceil(translation.x + scale.x);
		scissorRect.top = (int32_t)std::floor(translation.y);

		// default sprite and font placement:
		for (int i = IDLE; i < WIDGETSTATE_COUNT; ++i)
		{
			sprites[i].params.pos.x = translation.x;
			sprites[i].params.pos.y = translation.y;
			sprites[i].params.siz.x = scale.x;
			sprites[i].params.siz.y = scale.y;
			sprites[i].params.fade = enabled ? 0.0f : 0.5f;
		}
		font.params.posX = translation.x;
		font.params.posY = translation.y;

		hitBox = Hitbox2D(XMFLOAT2(translation.x, translation.y), XMFLOAT2(scale.x, scale.y));

		if (!force_disable && GetState() != WIDGETSTATE::ACTIVE && !tooltipFont.text.empty() && GetPointerHitbox().intersects(hitBox))
		{
			tooltipTimer++;
		}
		else
		{
			tooltipTimer = 0;
		}

		XMFLOAT2 highlight_pos = GetPointerHighlightPos(canvas);
		for (auto& x : sprites)
		{
			if (x.params.isHighlightEnabled())
			{
				x.params.highlight_pos = highlight_pos;
			}
		}

		sprites[state].Update(dt);
		font.Update(dt);
		angular_highlight_timer += dt;
	}
	void Widget::Render(const wi::Canvas& canvas, wi::graphics::CommandList cmd) const
	{
		if (!IsVisible())
		{
			return;
		}

		if (angular_highlight_width > 0)
		{
			wi::image::Params fx;
			fx.color = angular_highlight_color;
			fx.pos.x = translation.x - angular_highlight_width;
			fx.pos.y = translation.y - angular_highlight_width;
			fx.siz.x = scale.x + angular_highlight_width * 2;
			fx.siz.y = scale.y + angular_highlight_width * 2;
			if (sprites[state].params.isCornerRoundingEnabled())
			{
				fx.enableCornerRounding();
				fx.corners_rounding[0] = sprites[state].params.corners_rounding[0];
				fx.corners_rounding[1] = sprites[state].params.corners_rounding[1];
				fx.corners_rounding[2] = sprites[state].params.corners_rounding[2];
				fx.corners_rounding[3] = sprites[state].params.corners_rounding[3];
			}
			fx.angular_softness_outer_angle = XM_PI * 0.6f;
			fx.angular_softness_inner_angle = 0;
			XMStoreFloat2(&fx.angular_softness_direction, XMVector2Normalize(XMVectorSet(std::sin(angular_highlight_timer), std::cos(angular_highlight_timer), 0, 0)));
			fx.enableAngularSoftnessDoubleSided();
			fx.border_soften = 0.1f;
			wi::image::Draw(nullptr, fx, cmd);
		}
	}
	void Widget::RenderTooltip(const wi::Canvas& canvas, CommandList cmd) const
	{
		if (!IsVisible())
		{
			return;
		}

		if (tooltipTimer > 25)
		{
			float screenwidth = canvas.GetLogicalWidth();
			float screenheight = canvas.GetLogicalHeight();

			tooltipFont.params.position = {};
			tooltipFont.params.h_align = wi::font::WIFALIGN_LEFT;
			tooltipFont.params.v_align = wi::font::WIFALIGN_TOP;

			static const float _border = 2;
			XMFLOAT2 textSize = tooltipFont.TextSize();
			float textWidth = textSize.x;
			float textHeight = textSize.y;
			const float textHeightWithoutScriptTip = textHeight;

			if (!scripttipFont.text.empty())
			{
				XMFLOAT2 scriptTipSize = scripttipFont.TextSize();
				textWidth = std::max(textWidth, scriptTipSize.x);
				textHeight += scriptTipSize.y;
			}

			XMFLOAT2 pointer = GetPointerHitbox().pos;
			tooltipFont.params.posX = pointer.x;
			tooltipFont.params.posY = pointer.y;

			if (tooltipFont.params.posX + textWidth > screenwidth)
			{
				tooltipFont.params.posX -= tooltipFont.params.posX + textWidth - screenwidth;
			}
			if (tooltipFont.params.posX < 0)
			{
				tooltipFont.params.posX = 0;
			}

			if (tooltipFont.params.posY > screenheight * 0.8f)
			{
				tooltipFont.params.posY -= 30;
			}
			else
			{
				tooltipFont.params.posY += 40;
			}

			tooltipSprite.params.pos.x = tooltipFont.params.posX - _border;
			tooltipSprite.params.pos.y = tooltipFont.params.posY - _border;
			tooltipSprite.params.siz.x = textWidth + _border * 2;
			tooltipSprite.params.siz.y = textHeight + _border * 2;

			if (tooltip_shadow > 0)
			{
				wi::image::Params fx(
					tooltipSprite.params.pos.x - tooltip_shadow,
					tooltipSprite.params.pos.y - tooltip_shadow,
					tooltipSprite.params.siz.x + tooltip_shadow * 2,
					tooltipSprite.params.siz.y + tooltip_shadow * 2,
					tooltip_shadow_color
				);
				wi::image::Draw(nullptr, fx, cmd);
			}

			tooltipSprite.Draw(cmd);

			tooltipFont.Draw(cmd);

			if (!scripttipFont.text.empty())
			{
				scripttipFont.params = tooltipFont.params;
				scripttipFont.params.posY += textHeightWithoutScriptTip;
				scripttipFont.params.color = tooltipFont.params.color;
				scripttipFont.params.color.setA(uint8_t(float(scripttipFont.params.color.getA()) / 255.0f * 0.6f * 255));
				scripttipFont.Draw(cmd);
			}
		}
	}
	const std::string& Widget::GetName() const
	{
		return name;
	}
	void Widget::SetName(const std::string& value)
	{
		if (value.length() <= 0)
		{
			static std::atomic<uint32_t> widgetID{ 0 };
			name = "widget_" + std::to_string(widgetID.fetch_add(1));
		}
		else
		{
			name = value;
		}
	}
	std::string Widget::GetText() const
	{
		return font.GetTextA();
	}
	std::string Widget::GetTooltip() const
	{
		return tooltipFont.GetTextA();
	}
	void Widget::SetText(const char* value)
	{
		font.SetText(value);
	}
	void Widget::SetText(const std::string& value)
	{
		font.SetText(value);
	}
	void Widget::SetText(std::string&& value)
	{
		font.SetText(std::move(value));
	}
	void Widget::SetTooltip(const std::string& value)
	{
		tooltipFont.SetText(value);
	}
	void Widget::SetTooltip(std::string&& value)
	{
		tooltipFont.SetText(std::move(value));
	}
	void Widget::SetScriptTip(const std::string& value)
	{
		scripttipFont.SetText(value);
	}
	void Widget::SetScriptTip(std::string&& value)
	{
		scripttipFont.SetText(std::move(value));
	}
	void Widget::SetPos(const XMFLOAT2& value)
	{
		SetDirty();
		translation_local.x = value.x;
		translation_local.y = value.y;
		UpdateTransform();

		translation = translation_local;
	}
	void Widget::SetSize(const XMFLOAT2& value)
	{
		SetDirty();
		scale_local.x = value.x;
		scale_local.y = value.y;
		scale_local = wi::math::Max(scale_local, XMFLOAT3(0.001f, 0.001f, 0.001f));
		UpdateTransform();

		scale = scale_local;
	}
	XMFLOAT2 Widget::GetPos() const
	{
		return *(XMFLOAT2*)&translation;
	}
	XMFLOAT2 Widget::GetSize() const
	{
		return *(XMFLOAT2*)&scale;
	}
	WIDGETSTATE Widget::GetState() const
	{
		return state;
	}
	void Widget::SetEnabled(bool val)
	{
		if (enabled != val)
		{
			priority_change = val;
			enabled = val;
			if (!enabled)
				state = IDLE;
		}
	}
	bool Widget::IsEnabled() const
	{
		return enabled && visible && !force_disable;
	}
	void Widget::SetVisible(bool val)
	{
		if (visible != val)
		{
			priority_change |= val;
			visible = val;
			if (!visible)
				state = IDLE;
		}
	}
	bool Widget::IsVisible() const
	{
		if (parent != nullptr && !parent->IsVisible())
		{
			return false;
		}
		return visible;
	}
	void Widget::Activate()
	{
		priority_change = true;
		state = ACTIVE;
		tooltipTimer = 0;
	}
	void Widget::Deactivate()
	{
		state = DEACTIVATING;
		tooltipTimer = 0;
	}
	wi::Color Widget::GetColor() const
	{
		return wi::Color::fromFloat4(sprites[GetState()].params.color);
	}

	enum LOCALIZATION_ID
	{
		LOCALIZATION_ID_TEXT,
		LOCALIZATION_ID_TOOLTIP,
	};
	void Widget::ExportLocalization(wi::Localization& localization) const
	{
		if (!IsLocalizationEnabled())
			return;

		if (font.GetText().empty() && tooltipFont.GetText().empty())
			return;

		wi::Localization& section = localization.GetSection(GetName());
		section.SetSectionHint(GetWidgetTypeName());

		if (has_flag(localization_enabled, LocalizationEnabled::Text) && !font.GetText().empty())
		{
			section.Add(LOCALIZATION_ID_TEXT, font.GetTextA().c_str(), "text");
		}
		if (has_flag(localization_enabled, LocalizationEnabled::Tooltip) && !tooltipFont.GetText().empty())
		{
			section.Add(LOCALIZATION_ID_TOOLTIP, tooltipFont.GetTextA().c_str(), "tooltip");
		}
	}
	void Widget::ImportLocalization(const wi::Localization& localization)
	{
		const wi::Localization* section = localization.CheckSection(GetName());
		if (section == nullptr)
			return;

		if (has_flag(localization_enabled, LocalizationEnabled::Text))
		{
			const char* localized_text = section->Get(LOCALIZATION_ID_TEXT);
			if (localized_text != nullptr)
			{
				SetText(localized_text);
			}
		}
		if (has_flag(localization_enabled, LocalizationEnabled::Tooltip))
		{
			const char* localized_tooltip = section->Get(LOCALIZATION_ID_TOOLTIP);
			if (localized_tooltip != nullptr)
			{
				SetTooltip(localized_tooltip);
			}
		}
	}

	void Widget::SetColor(wi::Color color, int id)
	{
		if (id < 0)
		{
			for (int i = 0; i < arraysize(sprites); ++i)
			{
				sprites[i].params.color = color;
			}
		}
		else if (id < arraysize(sprites))
		{
			sprites[id].params.color = color;
		}
	}
	void Widget::SetShadowColor(wi::Color color)
	{
		shadow_color = color;
	}
	void Widget::SetImage(wi::Resource textureResource, int id)
	{
		if (id < 0)
		{
			for (int i = 0; i < arraysize(sprites); ++i)
			{
				sprites[i].textureResource = textureResource;
			}
		}
		else if (id < arraysize(sprites))
		{
			sprites[id].textureResource = textureResource;
		}
	}
	void Widget::SetTheme(const Theme& theme, int id)
	{
		if (id < 0)
		{
			for (int i = 0; i < arraysize(sprites); ++i)
			{
				theme.image.Apply(sprites[i].params);
			}
		}
		else if (id < arraysize(sprites))
		{
			theme.image.Apply(sprites[id].params);
		}
		theme.font.Apply(font.params);
		if (theme.shadow >= 0)
		{
			SetShadowRadius(theme.shadow);
		}
		SetShadowColor(theme.shadow_color);
		theme.tooltipFont.Apply(tooltipFont.params);
		theme.tooltipImage.Apply(tooltipSprite.params);
		if (theme.tooltip_shadow >= 0)
		{
			tooltip_shadow = theme.tooltip_shadow;
		}
		tooltip_shadow_color = theme.tooltip_shadow_color;
		shadow_highlight = theme.shadow_highlight;
		shadow_highlight_color = theme.shadow_highlight_color;
		shadow_highlight_spread = theme.shadow_highlight_spread;
		for (auto& x : sprites)
		{
			x.params.border_soften = theme.image.border_soften;
		}
	}

	void Widget::AttachTo(Widget* parent)
	{
		this->parent = parent;

		if (parent != nullptr)
		{
			parent->UpdateTransform();
			XMMATRIX B = XMMatrixInverse(nullptr, XMLoadFloat4x4(&parent->world));
			MatrixTransform(B);
		}

		UpdateTransform();

		if (parent != nullptr)
		{
			UpdateTransform_Parented(*parent);
		}

		XMVECTOR S, R, T;
		XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&world));
		XMStoreFloat3(&translation, T);
		XMStoreFloat3(&scale, S);
	}
	void Widget::Detach()
	{
		if (parent != nullptr)
		{
			parent = nullptr;
			ApplyTransform();
		}
	}
	void Widget::ApplyScissor(const wi::Canvas& canvas, const Rect rect, CommandList cmd, bool constrain_to_parent) const
	{
		Rect scissor = rect;

		if (constrain_to_parent && parent != nullptr)
		{
			Widget* recurse_parent = parent;
			while (recurse_parent != nullptr)
			{
				scissor.bottom = std::min(scissor.bottom, recurse_parent->scissorRect.bottom);
				scissor.top = std::max(scissor.top, recurse_parent->scissorRect.top);
				scissor.left = std::max(scissor.left, recurse_parent->scissorRect.left);
				scissor.right = std::min(scissor.right, recurse_parent->scissorRect.right);

				recurse_parent = recurse_parent->parent;
			}
		}

		if (scissor.left > scissor.right)
		{
			scissor.left = scissor.right;
		}
		if (scissor.top > scissor.bottom)
		{
			scissor.top = scissor.bottom;
		}

		GraphicsDevice* device = wi::graphics::GetDevice();
		float scale = canvas.GetDPIScaling();
		scissor.bottom = int32_t((float)scissor.bottom * scale);
		scissor.top = int32_t((float)scissor.top * scale);
		scissor.left = int32_t((float)scissor.left * scale);
		scissor.right = int32_t((float)scissor.right * scale);
		device->BindScissorRects(1, &scissor, cmd);
	}
	Hitbox2D Widget::GetPointerHitbox(bool constrained) const
	{
		XMFLOAT4 pointer = wi::input::GetPointer();
		Hitbox2D hb = Hitbox2D(XMFLOAT2(pointer.x, pointer.y), XMFLOAT2(1, 1));
		if (constrained)
		{
			HitboxConstrain(hb); // this is used to filter out pointer outside of active_area (outside of scissor basically)
		}
		return hb;
	}
	void Widget::HitboxConstrain(wi::primitive::Hitbox2D& hb) const
	{
		float left = hb.pos.x;
		float right = hb.pos.x + hb.siz.x;
		float top = hb.pos.y;
		float bottom = hb.pos.y + hb.siz.y;

		float area_left = active_area.pos.x;
		float area_right = active_area.pos.x + active_area.siz.x;
		float area_top = active_area.pos.y;
		float area_bottom = active_area.pos.y + active_area.siz.y;

		bottom = std::min(bottom, area_bottom);
		top = std::max(top, area_top);
		left = std::max(left, area_left);
		right = std::min(right, area_right);

		hb.pos.x = left;
		hb.pos.y = top;
		hb.siz.x = std::max(0.0f, right - left);
		hb.siz.y = std::max(0.0f, bottom - top);

		if (parent != nullptr)
		{
			parent->HitboxConstrain(hb);
		}
	}
	XMFLOAT2 Widget::GetPointerHighlightPos(const wi::Canvas& canvas) const
	{
		XMFLOAT4 pointer = wi::input::GetPointer();
		return XMFLOAT2(pointer.x / canvas.GetLogicalWidth(), pointer.y / canvas.GetLogicalHeight());
	}




	void Button::Create(const std::string& name)
	{
		SetName(name);
		SetText(name);
		OnClick([](EventArgs args) {});
		OnDragStart([](EventArgs args) {});
		OnDrag([](EventArgs args) {});
		OnDragEnd([](EventArgs args) {});
		SetSize(XMFLOAT2(100, 20));

		font.params.h_align = wi::font::WIFALIGN_CENTER;
		font.params.v_align = wi::font::WIFALIGN_CENTER;

		font_description.params = font.params;
		font_description.params.h_align = wi::font::WIFALIGN_RIGHT;
	}
	void Button::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		if (IsEnabled() && dt > 0)
		{
			hitBox.pos.x = translation.x;
			hitBox.pos.y = translation.y;
			hitBox.siz.x = scale.x;
			hitBox.siz.y = scale.y;

			Hitbox2D pointerHitbox = GetPointerHitbox();

			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				EventArgs args;
				args.clickPos = pointerHitbox.pos;
				onDragEnd(args);

				if (pointerHitbox.intersects(hitBox) && !disableClicking)
				{
					// Click occurs when the button is released within the bounds
					onClick(args);
				}

				state = IDLE;
			}
			if (state == ACTIVE)
			{
				Deactivate();
			}

			bool clicked = false;
			// hover the button
			if (pointerHitbox.intersects(hitBox))
			{
				if (state == IDLE)
				{
					state = FOCUS;
				}
			}

			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (state == FOCUS)
				{
					// activate
					clicked = true;
				}
			}

			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (state == DEACTIVATING)
				{
					// Keep pressed until mouse is released
					Activate();

					EventArgs args;
					args.clickPos = pointerHitbox.pos;
					XMFLOAT3 posDelta;
					posDelta.x = pointerHitbox.pos.x - prevPos.x;
					posDelta.y = pointerHitbox.pos.y - prevPos.y;
					posDelta.z = 0;
					args.deltaPos = XMFLOAT2(posDelta.x, posDelta.y);
					onDrag(args);
				}
			}

			if (clicked)
			{
				EventArgs args;
				args.clickPos = pointerHitbox.pos;
				dragStart = args.clickPos;
				args.startPos = dragStart;
				onDragStart(args);
				Activate();
			}

			prevPos.x = pointerHitbox.pos.x;
			prevPos.y = pointerHitbox.pos.y;
		}

		switch (font.params.h_align)
		{
		case wi::font::WIFALIGN_LEFT:
			font.params.posX = translation.x + 2;
			break;
		case wi::font::WIFALIGN_RIGHT:
			font.params.posX = translation.x + scale.x - 2;
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font.params.posX = translation.x + scale.x * 0.5f;
			break;
		}
		switch (font.params.v_align)
		{
		case wi::font::WIFALIGN_TOP:
			font.params.posY = translation.y + 2;
			break;
		case wi::font::WIFALIGN_BOTTOM:
			font.params.posY = translation.y + scale.y - 2;
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font.params.posY = translation.y + scale.y * 0.5f;
			break;
		}

		left_text_width = 0;
		right_text_width = 0;

		font_description.params.posX = translation.x;
		font_description.params.posY = translation.y + scale.y * 0.5f;
		switch (font_description.params.h_align)
		{
		case wi::font::WIFALIGN_LEFT:
			font_description.params.posX = translation.x + scale.x + shadow;
			right_text_width = font_description.TextWidth();
			break;
		case wi::font::WIFALIGN_RIGHT:
			font_description.params.posX = translation.x - shadow;
			left_text_width = font_description.TextWidth();
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font_description.params.posX = translation.x + scale.x * 0.5f;
			break;
		}
		switch (font_description.params.v_align)
		{
		case wi::font::WIFALIGN_TOP:
			font_description.params.posY = translation.y + scale.y + shadow;
			break;
		case wi::font::WIFALIGN_BOTTOM:
			font_description.params.posY = translation.y - shadow;
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font_description.params.posY = translation.y + scale.y * 0.5f;
			break;
		}

		if (state <= FOCUS)
		{
			disableClicking = false;
		}
	}
	void Button::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		font_description.Draw(cmd);

		ApplyScissor(canvas, scissorRect, cmd);

		sprites[state].Draw(cmd);
		font.Draw(cmd);
	}
	void Button::OnClick(std::function<void(EventArgs args)> func)
	{
		onClick = func;
	}
	void Button::OnDragStart(std::function<void(EventArgs args)> func)
	{
		onDragStart = func;
	}
	void Button::OnDrag(std::function<void(EventArgs args)> func)
	{
		onDrag = func;
	}
	void Button::OnDragEnd(std::function<void(EventArgs args)> func)
	{
		onDragEnd = func;
	}
	void Button::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		theme.font.Apply(font_description.params);
	}





	void ScrollBar::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		float scrollbar_begin;
		float scrollbar_end;
		float scrollbar_size;

		if (vertical)
		{
			scrollbar_begin = translation.y;
			scrollbar_end = scrollbar_begin + scale.y;
			scrollbar_size = scrollbar_end - scrollbar_begin;
			scrollbar_granularity = std::min(1.0f, scrollbar_size / std::max(1.0f, list_length - safe_area));
			scrollbar_length = std::max(scale.x * 2, scrollbar_size * scrollbar_granularity);
			scrollbar_length = std::min(scrollbar_length, scale.y);
		}
		else
		{
			scrollbar_begin = translation.x;
			scrollbar_end = scrollbar_begin + scale.x;
			scrollbar_size = scrollbar_end - scrollbar_begin;
			scrollbar_granularity = std::min(1.0f, scrollbar_size / std::max(1.0f, list_length - safe_area));
			scrollbar_length = std::max(scale.y * 2, scrollbar_size * scrollbar_granularity);
			scrollbar_length = std::min(scrollbar_length, scale.x);
		}
		scrollbar_length = std::max(0.0f, scrollbar_length);

		if (IsEnabled() && dt > 0)
		{
			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}
			if (state == ACTIVE)
			{
				Deactivate();
			}

			Hitbox2D pointerHitbox = GetPointerHitbox();

			if (state == IDLE && hitBox.intersects(pointerHitbox))
			{
				state = FOCUS;
			}

			bool clicked = false;
			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				clicked = true;
			}

			bool click_down = false;
			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				click_down = true;
				if (state == FOCUS || state == DEACTIVATING)
				{
					// Keep pressed until mouse is released
					Activate();
				}
			}

			if (!click_down)
			{
				scrollbar_state = SCROLLBAR_INACTIVE;
			}

			if (IsScrollbarRequired() && hitBox.intersects(pointerHitbox))
			{
				if (clicked)
				{
					scrollbar_state = SCROLLBAR_GRABBED;
					grab_pos = pointerHitbox.pos;
					grab_pos.x = wi::math::Clamp(grab_pos.x, scrollbar_begin + scrollbar_delta, scrollbar_begin + scrollbar_delta + scrollbar_length);
					grab_pos.y = wi::math::Clamp(grab_pos.y, scrollbar_begin + scrollbar_delta, scrollbar_begin + scrollbar_delta + scrollbar_length);
					grab_delta = scrollbar_delta;
				}
				else if (!click_down)
				{
					scrollbar_state = SCROLLBAR_HOVER;
					state = FOCUS;
				}
			}

			if (scrollbar_state == SCROLLBAR_GRABBED)
			{
				Activate();
				if (vertical)
				{
					scrollbar_delta = grab_delta + pointerHitbox.pos.y - grab_pos.y;
				}
				else
				{
					scrollbar_delta = grab_delta + pointerHitbox.pos.x - grab_pos.x;
				}
			}
		}

		scrollbar_delta = wi::math::Clamp(scrollbar_delta, 0, scrollbar_size - scrollbar_length);
		if (scrollbar_begin < scrollbar_end - scrollbar_length)
		{
			scrollbar_value = wi::math::InverseLerp(scrollbar_begin, scrollbar_end - scrollbar_length, scrollbar_begin + scrollbar_delta);
		}
		else
		{
			scrollbar_value = 0;
		}

		list_offset = -scrollbar_value * (list_length - scrollbar_size * (1.0f - overscroll));

		if (vertical)
		{
			for (int i = 0; i < arraysize(sprites_knob); ++i)
			{
				sprites_knob[i].params.pos.x = translation.x + knob_inset_border.x;
				sprites_knob[i].params.pos.y = translation.y + knob_inset_border.y + scrollbar_delta;
				sprites_knob[i].params.siz.x = std::max(0.0f, scale.x - knob_inset_border.x * 2);
				sprites_knob[i].params.siz.y = std::max(0.0f, scrollbar_length - knob_inset_border.y * 2);
			}
		}
		else
		{
			for (int i = 0; i < arraysize(sprites_knob); ++i)
			{
				sprites_knob[i].params.pos.x = translation.x + knob_inset_border.x + scrollbar_delta;
				sprites_knob[i].params.pos.y = translation.y + knob_inset_border.y;
				sprites_knob[i].params.siz.x = std::max(0.0f, scrollbar_length - knob_inset_border.x * 2);
				sprites_knob[i].params.siz.y = std::max(0.0f, scale.y - knob_inset_border.y * 2);
			}
		}

		if (!IsScrollbarRequired())
		{
			state = IDLE;
			list_offset = 0;
		}
	}
	void ScrollBar::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}
		if (!IsScrollbarRequired())
			return;

		// scrollbar background
		wi::image::Params fx = sprites[state].params;
		fx.pos = XMFLOAT3(translation.x, translation.y, 0);
		fx.siz = XMFLOAT2(scale.x, scale.y);
		fx.color = sprites[IDLE].params.color;
		wi::image::Draw(nullptr, fx, cmd);


		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites_knob[scrollbar_state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		// scrollbar knob
		sprites_knob[scrollbar_state].Draw(cmd);

		//Rect scissorRect;
		//scissorRect.bottom = (int32_t)(canvas.GetPhysicalHeight());
		//scissorRect.left = (int32_t)(0);
		//scissorRect.right = (int32_t)(canvas.GetPhysicalWidth());
		//scissorRect.top = (int32_t)(0);
		//GraphicsDevice* device = wi::graphics::GetDevice();
		//device->BindScissorRects(1, &scissorRect, cmd);
		//wi::image::Draw(nullptr, wi::image::Params(hitBox.pos.x, hitBox.pos.y, hitBox.siz.x, hitBox.siz.y, wi::Color(255,0,0,100)), cmd);

	}
	void ScrollBar::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);

		if (id > WIDGET_ID_SCROLLBAR_BEGIN && id < WIDGET_ID_SCROLLBAR_END)
		{
			if (id >= WIDGET_ID_SCROLLBAR_KNOB_INACTIVE)
			{
				sprites_knob[id - WIDGET_ID_SCROLLBAR_KNOB_INACTIVE].params.color = color;
			}
			else if (id >= WIDGET_ID_SCROLLBAR_BASE_IDLE)
			{
				sprites[id - WIDGET_ID_SCROLLBAR_BASE_IDLE].params.color = color;
			}
		}
	}
	void ScrollBar::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);

		if (id > WIDGET_ID_SCROLLBAR_BEGIN && id < WIDGET_ID_SCROLLBAR_END)
		{
			if (id >= WIDGET_ID_SCROLLBAR_KNOB_INACTIVE)
			{
				theme.image.Apply(sprites_knob[id - WIDGET_ID_SCROLLBAR_KNOB_INACTIVE].params);
			}
			else if (id >= WIDGET_ID_SCROLLBAR_BASE_IDLE)
			{
				theme.image.Apply(sprites[id - WIDGET_ID_SCROLLBAR_BASE_IDLE].params);
			}
		}
	}
	void ScrollBar::SetOffset(float value)
	{
		float scrollbar_begin;
		float scrollbar_end;
		float scrollbar_size;

		if (vertical)
		{
			scrollbar_begin = translation.y;
			scrollbar_end = scrollbar_begin + scale.y;
			scrollbar_size = scrollbar_end - scrollbar_begin;
		}
		else
		{
			scrollbar_begin = translation.x;
			scrollbar_end = scrollbar_begin + scale.x;
			scrollbar_size = scrollbar_end - scrollbar_begin;
		}

		scrollbar_delta = lerp(0.0f, scrollbar_size - scrollbar_length, value / list_length);
	}




	void Label::Create(const std::string& name)
	{
		SetName(name);
		SetText(name);
		SetSize(XMFLOAT2(100, 20));
		SetColor(GetColor()); // all states use same color by default

		scrollbar.SetColor(wi::Color(80, 80, 80, 100), wi::gui::IDLE);
		scrollbar.sprites_knob[ScrollBar::SCROLLBAR_INACTIVE].params.color = wi::Color(140, 140, 140, 140);
		scrollbar.sprites_knob[ScrollBar::SCROLLBAR_HOVER].params.color = wi::Color(180, 180, 180, 180);
		scrollbar.sprites_knob[ScrollBar::SCROLLBAR_GRABBED].params.color = wi::Color::White();
		scrollbar.SetOverScroll(0.25f);
		scrollbar.knob_inset_border = XMFLOAT2(4, 2);
	}
	void Label::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}
		Widget::Update(canvas, dt);

		if (wrap_enabled)
		{
			font.params.h_wrap = scale.x - margin_left - margin_right;
		}
		else
		{
			font.params.h_wrap = -1;
		}

		switch (font.params.h_align)
		{
		case wi::font::WIFALIGN_LEFT:
			font.params.posX = translation.x + 2 + margin_left;
			break;
		case wi::font::WIFALIGN_RIGHT:
			font.params.posX = translation.x + scale.x - 2 - margin_right;
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font.params.posX = translation.x + scale.x * 0.5f;
			break;
		}
		switch (font.params.v_align)
		{
		case wi::font::WIFALIGN_TOP:
			font.params.posY = translation.y + 2 + margin_top;
			break;
		case wi::font::WIFALIGN_BOTTOM:
			font.params.posY = translation.y + scale.y - 2 - margin_bottom;
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font.params.posY = translation.y + scale.y * 0.5f;
			break;
		}

		const float textheight = font.TextHeight();

		if (fittext_enabled)
		{
			SetSize(XMFLOAT2(GetSize().x, textheight + 4 + margin_top + margin_bottom));
		}

		if (scrollbar.IsEnabled())
		{
			scrollbar.SetListLength(textheight);
			scrollbar.ClearTransform();
			scrollbar.SetPos(XMFLOAT2(translation.x + scale.x - scrollbar_width, translation.y));
			scrollbar.SetSize(XMFLOAT2(scrollbar_width, scale.y));
			scrollbar.Update(canvas, dt);
		}

		if (IsEnabled() && dt > 0)
		{
			if (state == ACTIVE)
			{
				Deactivate();
			}

			Hitbox2D pointerHitbox = GetPointerHitbox();
			if (scroll_allowed && scrollbar.IsEnabled() && scrollbar.IsScrollbarRequired() && pointerHitbox.intersects(hitBox))
			{
				scroll_allowed = false;
				state = FOCUS;
				// This is outside scrollbar code, because it can also be scrolled if parent widget is only in focus
				scrollbar.Scroll(wi::input::GetPointer().z * 20);
			}
			else
			{
				state = IDLE;
			}

			if (pointerHitbox.intersects(hitBox) && wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				Activate();
			}
		}

		if (scrollbar.IsEnabled())
		{
			if (scrollbar.IsScrollbarRequired())
			{
				font.params.h_wrap = scale.x - scrollbar_width;
			}
			font.params.posY += scrollbar.GetOffset();
		}
	}
	void Label::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		ApplyScissor(canvas, scissorRect, cmd);

		sprites[IDLE].Draw(cmd);
		font.Draw(cmd);

		if (scrollbar.IsEnabled())
		{
			scrollbar.Render(canvas, cmd);
		}
	}
	void Label::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);
		scrollbar.SetColor(color, id);
	}
	void Label::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		scrollbar.SetTheme(theme, id);
	}




	wi::SpriteFont TextInputField::font_input;
	int caret_pos = 0;
	int caret_begin = 0;
	int caret_delay = 0;
	bool input_updated = false;
	wi::Timer caret_timer;
	void TextInputField::Create(const std::string& name)
	{
		SetName(name);
		SetText(name);
		OnInputAccepted([](EventArgs args) {});
		SetSize(XMFLOAT2(100, 20));

		font.params.v_align = wi::font::WIFALIGN_CENTER;

		font_description.params = font.params;
		font_description.params.h_align = wi::font::WIFALIGN_RIGHT;

		SetLocalizationEnabled(wi::gui::LocalizationEnabled::Tooltip); // disable localization of text because that can come from user input and musn't be overwritten!
	}
	void TextInputField::SetValue(const std::string& newValue)
	{
		font.SetText(newValue);
	}
	void TextInputField::SetValue(int newValue)
	{
		std::stringstream ss("");
		ss << newValue;
		font.SetText(ss.str());
	}
	void TextInputField::SetValue(float newValue)
	{
		if (newValue == FLT_MAX)
		{
			font.SetText(L"FLT_MAX");
		}
		else if (newValue == -FLT_MAX)
		{
			font.SetText(L"-FLT_MAX");
		}
		else
		{
			std::stringstream ss("");
			ss << newValue;
			font.SetText(ss.str());
		}
	}
	const std::string TextInputField::GetValue()
	{
		return font.GetTextA();
	}
	const std::string TextInputField::GetCurrentInputValue()
	{
		if (state == ACTIVE)
		{
			return font_input.GetTextA();
		}
		return GetValue();
	}
	void TextInputField::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		if (IsEnabled() && dt > 0)
		{
			hitBox.pos.x = translation.x;
			hitBox.pos.y = translation.y;
			hitBox.siz.x = scale.x;
			hitBox.siz.y = scale.y;

			Hitbox2D pointerHitbox = GetPointerHitbox();
			bool intersectsPointer = pointerHitbox.intersects(hitBox);

			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}

			// hover the button
			if (intersectsPointer)
			{
				if (state == IDLE)
				{
					state = FOCUS;
				}
			}

			bool clicked = false;
			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				clicked = true;
			}

			if (state == ACTIVE)
			{
				if (wi::input::Press(wi::input::KEYBOARD_BUTTON_ENTER))
				{
					// accept input...
					font.SetText(font_input.GetText());
					font_input.text.clear();

					if (onInputAccepted)
					{
						EventArgs args;
						args.sValue = font.GetTextA();
						args.iValue = atoi(args.sValue.c_str());
						args.fValue = (float)atof(args.sValue.c_str());
						onInputAccepted(args);
					}
					Deactivate();
				}
				//else if (wi::input::Press(wi::input::KEYBOARD_BUTTON_BACKSPACE))
				//{
				//	// delete input...
				//	DeleteFromInput(-1);
				//}
				else if (wi::input::Press(wi::input::KEYBOARD_BUTTON_DELETE))
				{
					// delete input...
					DeleteFromInput(1);
				}
				else if (wi::input::Press(wi::input::KEYBOARD_BUTTON_LEFT) && caret_pos > 0)
				{
					// caret repositioning left:
					caret_pos--;
					if (!wi::input::Down(wi::input::BUTTON::KEYBOARD_BUTTON_LSHIFT) && !wi::input::Down(wi::input::BUTTON::KEYBOARD_BUTTON_RSHIFT))
					{
						caret_begin = caret_pos;
					}
					caret_timer.record();
				}
				else if (wi::input::Press(wi::input::KEYBOARD_BUTTON_RIGHT) && caret_pos < font_input.GetText().size())
				{
					// caret repositioning right:
					caret_pos++;
					if (!wi::input::Down(wi::input::BUTTON::KEYBOARD_BUTTON_LSHIFT) && !wi::input::Down(wi::input::BUTTON::KEYBOARD_BUTTON_RSHIFT))
					{
						caret_begin = caret_pos;
					}
					caret_timer.record();
				}
				else if ((clicked && !intersectsPointer) || wi::input::Press(wi::input::KEYBOARD_BUTTON_ESCAPE))
				{
					// cancel input
					font_input.text.clear();
					Deactivate();
				}
				else if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
				{
					// caret repositioning by mouse click:
					caret_timer.record();
					caret_pos = (int)font_input.GetText().size();
					const std::wstring& str = font_input.GetText();
					float pos = font_input.params.position.x;
					for (size_t i = 0; i < str.size(); ++i)
					{
						XMFLOAT2 size = wi::font::TextSize(str.c_str() + i, 1, font_input.params);
						pos += size.x;
						if (pos > pointerHitbox.pos.x)
						{
							caret_pos = int(i);
							break;
						}
					}
					if (clicked && intersectsPointer)
					{
						caret_begin = caret_pos;
					}
					if (caret_delay < 1) // fix for bug: first click creates incorrect highlight state
					{
						caret_delay++;
						caret_begin = caret_pos;
					}
				}

				if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LCONTROL) && wi::input::Down((wi::input::BUTTON)'A'))
				{
					caret_begin = 0;
					caret_pos = (int)font_input.GetText().size();
				}

			}

			if (clicked && state == FOCUS)
			{
				// activate
				SetAsActive();
			}
		}

		left_text_width = 0;
		right_text_width = 0;

		font.params.posX = translation.x + 2;
		font.params.posY = translation.y + scale.y * 0.5f;
		font_description.params.posX = translation.x;
		font_description.params.posY = translation.y + scale.y * 0.5f;
		switch (font_description.params.h_align)
		{
		case wi::font::WIFALIGN_LEFT:
			font_description.params.posX = translation.x + scale.x + shadow;
			right_text_width = font_description.TextWidth();
			break;
		case wi::font::WIFALIGN_RIGHT:
			font_description.params.posX = translation.x - shadow;
			left_text_width = font_description.TextWidth();
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font_description.params.posX = translation.x + scale.x * 0.5f;
			break;
		}
		switch (font_description.params.v_align)
		{
		case wi::font::WIFALIGN_TOP:
			font_description.params.posY = translation.y + scale.y + shadow;
			break;
		case wi::font::WIFALIGN_BOTTOM:
			font_description.params.posY = translation.y - shadow;
			break;
		case wi::font::WIFALIGN_CENTER:
		default:
			font_description.params.posY = translation.y + scale.y * 0.5f;
			break;
		}

		if (state == ACTIVE)
		{
			font_input.params = font.params;

			if (!cancel_input_enabled)
			{
				SetValue(font_input.GetTextA());
			}

			if (input_updated && onInput)
			{
				wi::gui::EventArgs args;
				args.sValue = GetCurrentInputValue();
				onInput(args);
			}
			input_updated = false;
		}

	}
	void TextInputField::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		font_description.Draw(cmd);

		ApplyScissor(canvas, scissorRect, cmd);

		sprites[state].Draw(cmd);

		if (state == ACTIVE)
		{
			font_input.Draw(cmd);

			// caret:
			if(std::fmod(caret_timer.elapsed_seconds(), 1) < 0.5f)
			{
				wi::font::Params params = font_input.params;
				XMFLOAT2 size = wi::font::TextSize(font_input.GetText().c_str(), caret_pos, font_input.params);
				params.posX += size.x;
				params.color = wi::Color::lerp(params.color, wi::Color::Transparent(), 0.1f);
				params.size += 4;
				params.posY -= 2;
				params.h_align = wi::font::WIFALIGN_CENTER;
				wi::font::Draw(L"|", params, cmd);
			}

			// selection:
			if(caret_pos != caret_begin)
			{
				int start = std::min(caret_begin, caret_pos);
				int end = std::max(caret_begin, caret_pos);
				const std::wstring& str = font_input.GetText();
				float pos_start = font_input.params.position.x + wi::font::TextSize(str.c_str(), start, font_input.params).x;
				float pos_end = font_input.params.position.x + wi::font::TextSize(str.c_str(), end, font_input.params).x;
				float width = pos_end - pos_start;
				wi::image::Params params;
				params.pos.x = pos_start;
				params.pos.y = translation.y + 1;
				params.siz.x = width;
				params.siz.y = scale.y - 2;
				params.blendFlag = wi::enums::BLENDMODE_ALPHA;
				params.color = wi::Color::lerp(font_input.params.color, wi::Color::Transparent(), 0.5f);
				wi::image::Draw(nullptr, params, cmd);
			}

			//Rect scissorRect;
			//scissorRect.bottom = (int32_t)(canvas.GetPhysicalHeight());
			//scissorRect.left = (int32_t)(0);
			//scissorRect.right = (int32_t)(canvas.GetPhysicalWidth());
			//scissorRect.top = (int32_t)(0);
			//GraphicsDevice* device = wi::graphics::GetDevice();
			//device->BindScissorRects(1, &scissorRect, cmd);
			//wi::font::Draw("caret_begin = " + std::to_string(caret_begin) + "\ncaret_pos = " + std::to_string(caret_pos), wi::font::Params(0, 20), cmd);
		}
		else
		{
			font.Draw(cmd);
		}
	}
	void TextInputField::OnInputAccepted(std::function<void(EventArgs args)> func)
	{
		onInputAccepted = func;
	}
	void TextInputField::OnInput(std::function<void(EventArgs args)> func)
	{
		onInput = func;
	}
	void TextInputField::AddInput(const wchar_t inputChar)
	{
		input_updated = true;
		switch (inputChar)
		{
		case '\b':	// BACKSPACE
		case '\n':	// ENTER
		case '\r':	// ENTER
		case 127:	// DEL
			return;
		default:
			break;
		}
		std::wstring value_new = font_input.GetText();
		if (value_new.size() >= caret_pos)
		{
			int caret_pos_prev = caret_pos;
			if (caret_begin != caret_pos)
			{
				int offset = std::min(caret_pos, caret_begin);
				value_new.erase(offset, std::abs(caret_pos - caret_begin));
				caret_pos = offset;
			}
			int num = 0;
			if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LCONTROL) || wi::input::Down(wi::input::KEYBOARD_BUTTON_RCONTROL))
			{
				if (wi::input::Down((wi::input::BUTTON)'V'))
				{
					// Paste:
					std::wstring clipboard = wi::helper::GetClipboardText();
					value_new.insert(value_new.begin() + caret_pos, clipboard.begin(), clipboard.end());
					num = (int)clipboard.length();
				}
				else if (wi::input::Down((wi::input::BUTTON)'C'))
				{
					// Copy:
					caret_pos = caret_pos_prev;
					std::wstring text = font_input.GetText();
					int start = std::min(caret_begin, caret_pos);
					int end = std::max(caret_begin, caret_pos);
					std::wstring clipboard = std::wstring(text.c_str() + start, text.c_str() + end);
					wi::helper::SetClipboardText(clipboard);
					return;
				}
				else if (wi::input::Down((wi::input::BUTTON)'X'))
				{
					// Cut:
					caret_pos = caret_pos_prev;
					std::wstring text = font_input.GetText();
					int start = std::min(caret_begin, caret_pos);
					int end = std::max(caret_begin, caret_pos);
					std::wstring clipboard = std::wstring(text.c_str() + start, text.c_str() + end);
					wi::helper::SetClipboardText(clipboard);
				}
				else
					return;
			}
			else
			{
				value_new.insert(value_new.begin() + caret_pos, inputChar);
				num = 1;
			}
			font_input.SetText(value_new);
			caret_pos = std::min((int)font_input.GetText().size(), caret_pos + num);
		}
		caret_begin = caret_pos;
	}
	void TextInputField::AddInput(const char inputChar)
	{
		AddInput((wchar_t)inputChar);
	}
	void TextInputField::DeleteFromInput(int direction)
	{
		input_updated = true;
		std::wstring value_new = font_input.GetText();
		if (caret_begin != caret_pos)
		{
			int offset = std::min(caret_pos, caret_begin);
			value_new.erase(offset, std::abs(caret_pos - caret_begin));
			caret_pos = offset;
		}
		else
		{
			if (direction < 0)
			{
				if (caret_pos > 0 && value_new.size() > caret_pos - 1)
				{
					value_new.erase(value_new.begin() + caret_pos - 1);
					caret_pos = std::max(0, caret_pos - 1);
				}
			}
			else
			{
				if (value_new.size() > caret_pos)
				{
					value_new.erase(value_new.begin() + caret_pos);
					caret_pos = std::min((int)value_new.size(), caret_pos);
				}
			}
		}
		caret_begin = caret_pos;
		font_input.SetText(value_new);
	}
	void TextInputField::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);

		if (id > WIDGET_ID_TEXTINPUTFIELD_BEGIN && id < WIDGET_ID_TEXTINPUTFIELD_END)
		{
			sprites[id - WIDGET_ID_TEXTINPUTFIELD_IDLE].params.color = color;
		}
	}
	void TextInputField::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		theme.font.Apply(font_description.params);
	}
	void TextInputField::SetAsActive(bool selectall)
	{
		Activate();
		font_input.SetText(font.GetText());
		caret_pos = (int)font_input.GetText().size();
		caret_begin = selectall ? 0 : caret_pos;
		caret_delay = 0;
	}
	void TextInputField::Activate()
	{
		if (state != ACTIVE)
			typing_active = true;
		Widget::Activate();
	}
	void TextInputField::Deactivate()
	{
		if(state == ACTIVE)
			typing_active = false;
		Widget::Deactivate();
	}




	void Slider::Create(float start, float end, float defaultValue, float step, const std::string& name)
	{
		this->start = start;
		this->end = end;
		this->value = defaultValue;
		this->step = std::max(step, 1.0f);

		SetName(name);
		SetText(name);
		OnSlide([](EventArgs args) {});
		SetSize(XMFLOAT2(200, 20));

		valueInputField.Create(name + "_endInputField");
		valueInputField.SetLocalizationEnabled(LocalizationEnabled::None);
		valueInputField.SetShadowRadius(0);
		valueInputField.SetTooltip("Enter number to modify value even outside slider limits. Other inputs:\n - reset : reset slider to initial state.\n - FLT_MAX : float max value\n - -FLT_MAX : negative float max value.");
		valueInputField.SetValue(end);
		valueInputField.OnInputAccepted([this, start, end, defaultValue](EventArgs args) {
			if (args.sValue.compare("reset") == 0)
			{
				this->value = defaultValue;
				this->start = start;
				this->end = end;
				args.fValue = this->value;
				args.iValue = (int)this->value;
			}
			else if (args.sValue.compare("FLT_MAX") == 0)
			{
				this->value = FLT_MAX;
				args.fValue = this->value;
				args.iValue = (int)this->value;
			}
			else if (args.sValue.compare("-FLT_MAX") == 0)
			{
				this->value = -FLT_MAX;
				args.fValue = this->value;
				args.iValue = (int)this->value;
			}
			else
			{
				this->value = args.fValue;
				this->start = std::min(this->start, args.fValue);
				this->end = std::max(this->end, args.fValue);
			}
			onSlide(args);
			});

		for (int i = IDLE; i < WIDGETSTATE_COUNT; ++i)
		{
			sprites_knob[i].params = sprites[i].params;
		}

		sprites[IDLE].params.color = wi::Color(60, 60, 60, 200);
		sprites[FOCUS].params.color = wi::Color(50, 50, 50, 200);
		sprites[ACTIVE].params.color = wi::Color(50, 50, 50, 200);
		sprites[DEACTIVATING].params.color = wi::Color(60, 60, 60, 200);

		font.params.h_align = wi::font::WIFALIGN_RIGHT;
		font.params.v_align = wi::font::WIFALIGN_CENTER;
	}
	void Slider::SetValue(float value)
	{
		this->value = value;
	}
	void Slider::SetValue(int value)
	{
		this->value = float(value);
	}
	float Slider::GetValue() const
	{
		return value;
	}
	void Slider::SetRange(float start, float end)
	{
		this->start = start;
		this->end = end;
		this->value = wi::math::Clamp(this->value, start, end);
	}
	void Slider::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		valueInputField.Detach();
		if (state != ACTIVE)
		{
			// only set input field size when slider is not dragged because now it will modify slider active size too, and slider size shouldn't be modified while dragging!
			valueInputField.SetSize(XMFLOAT2(std::max(scale.y, wi::font::TextWidth(valueInputField.GetCurrentInputValue(), valueInputField.font.params) + 4), scale.y));
			valueInputField.SetPos(XMFLOAT2(translation.x + scale.x - valueInputField.GetSize().x, translation.y));
		}
		valueInputField.AttachTo(this);

		hitBox.siz.x = scale.x - valueInputField.GetSize().x - 1;

		scissorRect.bottom = (int32_t)std::ceil(translation.y + scale.y);
		scissorRect.left = (int32_t)std::floor(translation.x);
		scissorRect.right = (int32_t)std::ceil(translation.x + scale.x);
		scissorRect.top = (int32_t)std::floor(translation.y);

		for (int i = 0; i < WIDGETSTATE_COUNT; ++i)
		{
			sprites_knob[i].params.siz.x = 16.0f;
		}
		valueInputField.SetEnabled(enabled);
		valueInputField.force_disable = force_disable;
		valueInputField.Update(canvas, dt);

		if (IsEnabled() && dt > 0)
		{
			bool dragged = false;

			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}
			if (state == ACTIVE)
			{
				if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
				{
					if (state == ACTIVE)
					{
						// continue drag if already grabbed whether it is intersecting or not
						dragged = true;
					}
				}
				else
				{
					Deactivate();
				}
			}

			Hitbox2D pointerHitbox = GetPointerHitbox();

			if (pointerHitbox.intersects(hitBox))
			{
				// hover the slider
				if (state == IDLE)
				{
					state = FOCUS;
				}
			}

			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (state == FOCUS)
				{
					// activate
					dragged = true;
				}
			}


			if (dragged)
			{
				EventArgs args;
				args.clickPos = pointerHitbox.pos;
				value = wi::math::InverseLerp(translation.x, translation.x + hitBox.siz.x, args.clickPos.x);
				value = wi::math::Clamp(value, 0, 1);
				value *= step;
				value = std::floor(value);
				value /= step;
				value = wi::math::Lerp(start, end, value);
				args.fValue = value;
				args.iValue = (int)value;
				onSlide(args);
				Activate();
			}
		}

		valueInputField.SetValue(value);

		font.params.posY = translation.y + scale.y * 0.5f;

		const float knobWidth = sprites_knob[state].params.siz.x;
		const float knobWidthHalf = knobWidth * 0.5f;
		sprites_knob[state].params.pos.x = wi::math::Lerp(translation.x + knobWidthHalf + 2, translation.x + hitBox.siz.x - knobWidthHalf - 2, wi::math::Clamp(wi::math::InverseLerp(start, end, value), 0, 1));
		sprites_knob[state].params.pos.y = translation.y + 2;
		sprites_knob[state].params.siz.y = scale.y - 4;
		sprites_knob[state].params.pivot = XMFLOAT2(0.5f, 0);
		sprites_knob[state].params.fade = sprites[state].params.fade;

		sprites[state].params.siz.x = hitBox.siz.x;

		left_text_width = font.TextWidth();
	}
	void Slider::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x = scale.x;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		font.Draw(cmd);

		ApplyScissor(canvas, scissorRect, cmd);

		// base
		sprites[state].Draw(cmd);

		// knob
		sprites_knob[state].Draw(cmd);

		// input field
		valueInputField.Render(canvas, cmd);
	}
	void Slider::RenderTooltip(const wi::Canvas& canvas, wi::graphics::CommandList cmd) const
	{
		Widget::RenderTooltip(canvas, cmd);
		valueInputField.RenderTooltip(canvas, cmd);
	}
	void Slider::OnSlide(std::function<void(EventArgs args)> func)
	{
		onSlide = func;
	}
	void Slider::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);
		valueInputField.SetColor(color, id);

		if (id > WIDGET_ID_SLIDER_BEGIN && id < WIDGET_ID_SLIDER_END)
		{
			if (id >= WIDGET_ID_SLIDER_KNOB_IDLE)
			{
				sprites_knob[id - WIDGET_ID_SLIDER_KNOB_IDLE].params.color = color;
			}
			else if (id >= WIDGET_ID_SLIDER_BASE_IDLE)
			{
				sprites[id - WIDGET_ID_SLIDER_BASE_IDLE].params.color = color;
			}
		}
	}
	void Slider::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		valueInputField.SetTheme(theme, id);

		if (id > WIDGET_ID_SLIDER_BEGIN && id < WIDGET_ID_SLIDER_END)
		{
			if (id >= WIDGET_ID_SLIDER_KNOB_IDLE)
			{
				theme.image.Apply(sprites_knob[id - WIDGET_ID_SLIDER_KNOB_IDLE].params);
			}
			else if (id >= WIDGET_ID_SLIDER_BASE_IDLE)
			{
				theme.image.Apply(sprites[id - WIDGET_ID_SLIDER_BASE_IDLE].params);
			}
		}
	}




	std::wstring check_text_global;
	void CheckBox::Create(const std::string& name)
	{
		SetName(name);
		SetText(name);
		OnClick([](EventArgs args) {});
		SetSize(XMFLOAT2(20, 20));

		font.params.h_align = wi::font::WIFALIGN_RIGHT;
		font.params.v_align = wi::font::WIFALIGN_CENTER;
	}
	void CheckBox::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		if (IsEnabled() && dt > 0)
		{

			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}
			if (state == ACTIVE)
			{
				Deactivate();
			}

			hitBox.pos.x = translation.x;
			hitBox.pos.y = translation.y;
			hitBox.siz.x = scale.x;
			hitBox.siz.y = scale.y;

			Hitbox2D pointerHitbox = GetPointerHitbox();

			// hover the button
			if (pointerHitbox.intersects(hitBox))
			{
				if (state == IDLE)
				{
					state = FOCUS;
				}
			}

			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (state == FOCUS)
				{
					Activate();
				}
			}

			if (state == DEACTIVATING)
			{
				if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
				{
					// Keep pressed until mouse is released
					Activate();
				}
				else
				{
					// Deactivation event
					SetCheck(!GetCheck());
					EventArgs args;
					args.clickPos = pointerHitbox.pos;
					args.bValue = GetCheck();
					onClick(args);
				}
			}
		}

		font.params.posY = translation.y + scale.y * 0.5f;

		left_text_width = font.TextWidth();
	}
	void CheckBox::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		font.Draw(cmd);

		ApplyScissor(canvas, scissorRect, cmd);

		// control
		sprites[state].Draw(cmd);

		// check
		if (GetCheck())
		{
			if (!check_text.empty())
			{
				// render text symbol:
				wi::font::Params params;
				params.posX = translation.x + scale.x * 0.5f;
				params.posY = translation.y + scale.y * 0.5f;
				params.h_align = wi::font::WIFALIGN_CENTER;
				params.v_align = wi::font::WIFALIGN_CENTER;
				params.size = int(scale.y);
				params.scaling = 0.75f;
				params.color = font.params.color;
				wi::font::Draw(check_text, params, cmd);
			}
			else if (!check_text_global.empty())
			{
				// render text symbol:
				wi::font::Params params;
				params.posX = translation.x + scale.x * 0.5f;
				params.posY = translation.y + scale.y * 0.5f;
				params.h_align = wi::font::WIFALIGN_CENTER;
				params.v_align = wi::font::WIFALIGN_CENTER;
				params.size = int(scale.y);
				params.scaling = 0.75f;
				params.color = font.params.color;
				wi::font::Draw(check_text_global, params, cmd);
			}
			else
			{
				// simple square:
				wi::image::Params params(
					translation.x + scale.x * 0.25f,
					translation.y + scale.y * 0.25f,
					scale.x * 0.5f,
					scale.y * 0.5f
				);
				params.color = font.params.color;
				wi::image::Draw(nullptr, params, cmd);
			}
		}
		else if (!uncheck_text.empty())
		{
			wi::font::Params params;
			params.posX = translation.x + scale.x * 0.5f;
			params.posY = translation.y + scale.y * 0.5f;
			params.h_align = wi::font::WIFALIGN_CENTER;
			params.v_align = wi::font::WIFALIGN_CENTER;
			params.size = int(scale.y);
			params.scaling = 0.75f;
			params.color = font.params.color;
			wi::font::Draw(uncheck_text, params, cmd);
		}

	}
	void CheckBox::OnClick(std::function<void(EventArgs args)> func)
	{
		onClick = func;
	}
	void CheckBox::SetCheck(bool value)
	{
		checked = value;
	}
	bool CheckBox::GetCheck() const
	{
		return checked;
	}

	void CheckBox::SetCheckTextGlobal(const std::string& text)
	{
		wi::helper::StringConvert(text, check_text_global);
	}
	void CheckBox::SetCheckText(const std::string& text)
	{
		wi::helper::StringConvert(text, check_text);
	}
	void CheckBox::SetUnCheckText(const std::string& text)
	{
		wi::helper::StringConvert(text, uncheck_text);
	}





	static constexpr float combo_height() { return 20; };
	void ComboBox::Create(const std::string& name)
	{
		SetName(name);
		SetText(name);
		OnSelect([](EventArgs args) {});
		SetSize(XMFLOAT2(100, 20));

		font.params.h_align = wi::font::WIFALIGN_RIGHT;
		font.params.v_align = wi::font::WIFALIGN_CENTER;

		selected_font = font;
		selected_font.params.h_align = wi::font::WIFALIGN_CENTER;
		selected_font.params.v_align = wi::font::WIFALIGN_CENTER;

		filter.Create("");
		filter.SetCancelInputEnabled(false);
	}
	float ComboBox::GetDropOffset(const wi::Canvas& canvas) const
	{
		float screenheight = canvas.GetLogicalHeight();
		int visible_items = std::min(maxVisibleItemCount, filteredItemCount - firstItemVisible);
		if (!filterText.empty())
		{
			visible_items = 0;
			for (int i = firstItemVisible; (i < (int)items.size()) && (visible_items < maxVisibleItemCount); ++i)
			{
				if (wi::helper::toUpper(items[i].name).find(filterText) != std::string::npos)
				{
					visible_items++;
				}
			}
		}
		float total_height = scale.y + visible_items * combo_height();
		if (translation.y + total_height > screenheight)
		{
			return -total_height - 1;
		}
		return combo_height(); // space for filter sub-widget above
	}
	float ComboBox::GetDropX(const wi::Canvas& canvas) const
	{
		if (fixed_drop_width && translation.x > canvas.GetLogicalWidth() * 0.5f)
		{
			// in this case, left-align the drop down:
			float x = translation.x + scale.x - fixed_drop_width;
			if (HasScrollbar())
			{
				x -= 1 + scale.y;
			}
			x = std::max(0.0f, x);
			return x;
		}
		return translation.x;
	}
	float ComboBox::GetItemOffset(const wi::Canvas& canvas, int index) const
	{
		if (!filterText.empty())
		{
			int invisible_items = 0;
			for (int i = firstItemVisible; (i < (int)items.size()) && (i < index); ++i)
			{
				if (wi::helper::toUpper(items[i].name).find(filterText) == std::string::npos)
				{
					invisible_items++;
				}
			}
			index -= invisible_items;
		}
		index = std::max(firstItemVisible, index) - firstItemVisible;
		return scale.y + combo_height() * index + GetDropOffset(canvas);
	}
	bool ComboBox::HasScrollbar() const
	{
		return maxVisibleItemCount < filteredItemCount;
	}
	void ComboBox::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		const float drop_width = fixed_drop_width > 0 ? fixed_drop_width : (scale.x - 1 - scale.y);
		const float drop_x = GetDropX(canvas);

		if (IsEnabled() && dt > 0)
		{
			float drop_offset = GetDropOffset(canvas);

			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}
			if (state == ACTIVE && combostate == COMBOSTATE_SELECTING)
			{
				hovered = -1;
				Deactivate();
			}
			if (state == IDLE)
			{
				combostate = COMBOSTATE_INACTIVE;
			}

			hitBox.pos.x = translation.x;
			hitBox.pos.y = translation.y;
			hitBox.siz.x = scale.x;
			if (drop_arrow)
			{
				hitBox.siz.x += scale.y + 1; // + drop-down indicator arrow + little offset
			}
			hitBox.siz.y = scale.y;

			Hitbox2D pointerHitbox = GetPointerHitbox();

			bool clicked = false;
			// hover the button
			if (pointerHitbox.intersects(hitBox))
			{
				if (state == IDLE)
				{
					state = FOCUS;
				}
			}

			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				// activate
				clicked = true;
			}

			bool click_down = false;
			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				click_down = true;
				if (state == DEACTIVATING)
				{
					// Keep pressed until mouse is released
					Activate();
				}
			}


			if (clicked && state == FOCUS)
			{
				Activate();
			}

			if (state == ACTIVE)
			{
				filteredItemCount = int(items.size());
				if (!filterText.empty())
				{
					filteredItemCount = 0;
					for (int i = 0; i < (int)items.size(); ++i)
					{
						if (wi::helper::toUpper(items[i].name).find(filterText) == std::string::npos)
							continue;
						filteredItemCount++;
					}
				}

				const float scrollbar_begin = translation.y + scale.y + drop_offset + scale.y * 0.5f;
				const float scrollbar_end = scrollbar_begin + std::max(0.0f, (float)std::min(maxVisibleItemCount, filteredItemCount) - 1) * combo_height();

				pointerHitbox = GetPointerHitbox(false); // get the hitbox again, but this time it won't be constrained to parent
				if (HasScrollbar())
				{
					if (combostate != COMBOSTATE_SELECTING && combostate != COMBOSTATE_INACTIVE)
					{
						if (combostate == COMBOSTATE_SCROLLBAR_GRABBED || pointerHitbox.intersects(Hitbox2D(XMFLOAT2(drop_x + drop_width + 1, translation.y + scale.y + drop_offset), XMFLOAT2(scale.y, (float)std::min(maxVisibleItemCount, filteredItemCount) * combo_height()))))
						{
							if (click_down)
							{
								filter.SetAsActive();
								combostate = COMBOSTATE_SCROLLBAR_GRABBED;
								scrollbar_delta = wi::math::Clamp(pointerHitbox.pos.y, scrollbar_begin, scrollbar_end) - scrollbar_begin;
								const float scrollbar_value = wi::math::InverseLerp(scrollbar_begin, scrollbar_end, scrollbar_begin + scrollbar_delta);
								firstItemVisible = int(float(std::max(0, filteredItemCount - maxVisibleItemCount)) * scrollbar_value + 0.5f);
								firstItemVisible = std::max(0, std::min(filteredItemCount - maxVisibleItemCount, firstItemVisible));
							}
							else
							{
								combostate = COMBOSTATE_SCROLLBAR_HOVER;
							}
						}
						else if (!click_down)
						{
							combostate = COMBOSTATE_HOVER;
						}
					}
				}

				if (combostate == COMBOSTATE_INACTIVE)
				{
					combostate = COMBOSTATE_HOVER;
					filter.SetAsActive();
				}
				else if (combostate == COMBOSTATE_SELECTING || wi::input::Press(wi::input::KEYBOARD_BUTTON_ESCAPE))
				{
					Deactivate();
					combostate = COMBOSTATE_INACTIVE;
				}
				else if (combostate == COMBOSTATE_HOVER && scroll_allowed)
				{
					scroll_allowed = false;

					if (HasScrollbar())
					{
						int scroll = (int)wi::input::GetPointer().z;
						firstItemVisible -= scroll;
						firstItemVisible = std::max(0, std::min(filteredItemCount - maxVisibleItemCount, firstItemVisible));
						if (scroll)
						{
							const float scrollbar_value = wi::math::InverseLerp(0, float(std::max(0, filteredItemCount - maxVisibleItemCount)), float(firstItemVisible));
							scrollbar_delta = wi::math::Lerp(scrollbar_begin, scrollbar_end, scrollbar_value) - scrollbar_begin;
						}
					}

					hovered = -1;
					int visible_items = 0;
					for (int i = firstItemVisible; (i < (int)items.size()) && (visible_items < maxVisibleItemCount); ++i)
					{
						if (!filterText.empty() && wi::helper::toUpper(items[i].name).find(filterText) == std::string::npos)
							continue;
						visible_items++;
						Hitbox2D itembox;
						itembox.pos.x = drop_x;
						itembox.pos.y = translation.y + GetItemOffset(canvas, i);
						itembox.siz.x = drop_width;
						itembox.siz.y = combo_height();
						if (pointerHitbox.intersects(itembox))
						{
							hovered = i;
							break;
						}
					}

					if (clicked)
					{
						if (pointerHitbox.intersects(filter.hitBox))
						{
							combostate = COMBOSTATE_FILTER_INTERACT;
						}
						else
						{
							combostate = COMBOSTATE_SELECTING;
							if (hovered >= 0)
							{
								SetSelected(hovered);
							}
						}
					}
				}
				else if (combostate == COMBOSTATE_FILTER_INTERACT)
				{
					// nothing here, but this holds main widget active while filter interaction is detected
					if (clicked && !pointerHitbox.intersects(filter.hitBox))
					{
						combostate = COMBOSTATE_INACTIVE;
					}
				}
			}

			if (state == ACTIVE) // intentionally checks base state again!
			{
				filter.Activate();
				filter.scale_local.x = drop_width;
				filter.scale_local.y = combo_height() - filter.GetShadowRadius() * 2;
				filter.translation_local.x = drop_x;
				filter.translation_local.y = translation.y + scale.y + drop_offset - combo_height() + filter.GetShadowRadius();
				filter.SetDirty();
				filterText = wi::helper::toUpper(filter.GetText());
				filter.font.params.size = int(filter.scale_local.y - 4);
			}
			else
			{
				filter.Deactivate();
				filter.SetText("");
				filterText = "";
			}
		}
		else
		{
			filter.Deactivate();
		}

		filter.SetEnabled(enabled);
		filter.force_disable = force_disable;
		filter.Update(canvas, dt);

		sprites[state].params.siz.x = scale.x;
		if (IsDropArrowEnabled())
		{
			sprites[state].params.siz.x -= scale.y + 1;
		}

		font.params.posY = translation.y + sprites[state].params.siz.y * 0.5f;

		selected = std::min((int)items.size(), selected);

		if (selected >= 0)
		{
			selected_font.SetText(items[selected].name);
		}
		else
		{
			selected_font.SetText(invalid_selection_text);
		}
		selected_font.params.posX = translation.x + sprites[state].params.siz.x * 0.5f;
		selected_font.params.posY = translation.y + scale.y * 0.5f;
		selected_font.Update(dt);

		scissorRect.bottom = (int32_t)std::ceil(translation.y + scale.y);
		scissorRect.left = (int32_t)std::floor(translation.x);
		scissorRect.right = (int32_t)std::ceil(translation.x + sprites[state].params.siz.x);
		scissorRect.top = (int32_t)std::floor(translation.y);

		left_text_width = font.TextWidth();
	}
	void ComboBox::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}
		GraphicsDevice* device = wi::graphics::GetDevice();

		const float drop_width = fixed_drop_width > 0 ? fixed_drop_width : (scale.x - 1 - scale.y);
		const float drop_x = GetDropX(canvas);

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x = scale.x;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		wi::Color color = GetColor();
		if (combostate != COMBOSTATE_INACTIVE)
		{
			color = wi::Color::fromFloat4(sprites[FOCUS].params.color);
		}

		font.Draw(cmd);

		const float drop_offset = GetDropOffset(canvas);

		if (drop_arrow)
		{
			struct Vertex
			{
				XMFLOAT4 pos;
				XMFLOAT4 col;
			};
			static GPUBuffer vb_triangle;
			if (!vb_triangle.IsValid())
			{
				Vertex vertices[3];
				vertices[0].col = XMFLOAT4(1, 1, 1, 1);
				vertices[1].col = XMFLOAT4(1, 1, 1, 1);
				vertices[2].col = XMFLOAT4(1, 1, 1, 1);
				wi::math::ConstructTriangleEquilateral(1, vertices[0].pos, vertices[1].pos, vertices[2].pos);

				GPUBufferDesc desc;
				desc.bind_flags = BindFlag::VERTEX_BUFFER;
				desc.size = sizeof(vertices);
				device->CreateBuffer(&desc, &vertices, &vb_triangle);
			}
			const XMMATRIX Projection = canvas.GetProjection();

			// control-arrow-background
			wi::image::Params fx = sprites[state].params;
			fx.disableCornerRounding();
			fx.pos = XMFLOAT3(translation.x + scale.x - scale.y, translation.y, 0);
			fx.siz = XMFLOAT2(scale.y, scale.y);
			wi::image::Draw(nullptr, fx, cmd);

			// control-arrow-triangle
			{
				device->BindPipelineState(&gui_internal().PSO_colored, cmd);

				MiscCB cb;
				cb.g_xColor = font.params.color;
				XMStoreFloat4x4(&cb.g_xTransform, XMMatrixScaling(scale.y * 0.25f, scale.y * 0.25f, 1) *
					XMMatrixRotationZ(drop_offset < 0 ? -XM_PIDIV2 : XM_PIDIV2) *
					XMMatrixTranslation(translation.x + scale.x - scale.y * 0.5f, translation.y + scale.y * 0.5f, 0) *
					Projection
				);
				device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
				const GPUBuffer* vbs[] = {
					&vb_triangle,
				};
				const uint32_t strides[] = {
					sizeof(Vertex),
				};
				device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);

				device->Draw(3, 0, cmd);
			}
		}

		ApplyScissor(canvas, scissorRect, cmd);

		// control-base
		sprites[state].Draw(cmd);

		selected_font.Draw(cmd);

		// drop-down
		if (state == ACTIVE)
		{
			{
				Rect fullscissorRect;
				fullscissorRect.bottom = (int32_t)(canvas.GetPhysicalHeight());
				fullscissorRect.left = (int32_t)(0);
				fullscissorRect.right = (int32_t)(canvas.GetPhysicalWidth());
				fullscissorRect.top = (int32_t)(0);
				ApplyScissor(canvas, fullscissorRect, cmd);
				filter.Render(canvas, cmd);
			}

			if (HasScrollbar())
			{
				Rect rect;
				rect.left = int(drop_x + drop_width + 1);
				rect.right = int(drop_x + drop_width + 1 + scale.y);
				rect.top = int(translation.y + scale.y + drop_offset);
				rect.bottom = int(translation.y + scale.y + drop_offset + combo_height() * maxVisibleItemCount);
				ApplyScissor(canvas, rect, cmd, false);

				// control-scrollbar-base
				{
					wi::image::Params fx = sprites[state].params;
					fx.disableCornerRounding();
					fx.pos = XMFLOAT3(drop_x + drop_width + 1, translation.y + scale.y + drop_offset, 0);
					fx.siz = XMFLOAT2(scale.y, combo_height() * maxVisibleItemCount);
					fx.color = drop_color;
					wi::image::Draw(nullptr, fx, cmd);
				}

				// control-scrollbar-grab
				{
					wi::Color col = wi::Color::fromFloat4(sprites[IDLE].params.color);
					if (combostate == COMBOSTATE_SCROLLBAR_HOVER)
					{
						col = wi::Color::fromFloat4(sprites[FOCUS].params.color);
					}
					else if (combostate == COMBOSTATE_SCROLLBAR_GRABBED)
					{
						col = wi::Color::fromFloat4(sprites[ACTIVE].params.color);
					}
					wi::image::Draw(
						nullptr,
						wi::image::Params(
							drop_x + drop_width + 1,
							translation.y + scale.y + drop_offset + scrollbar_delta,
							scale.y,
							combo_height(),
							col
						),
						cmd
					);
				}
			}

			Rect rect;
			rect.left = int(drop_x);
			rect.right = rect.left + int(drop_width);
			rect.top = int(translation.y + scale.y + drop_offset);
			rect.bottom = rect.top + int(combo_height() * maxVisibleItemCount);
			ApplyScissor(canvas, rect, cmd, false);

			// control-list
			int visible_items = 0;
			for (int i = firstItemVisible; (i < (int)items.size()) && (visible_items < maxVisibleItemCount); ++i)
			{
				if (!filterText.empty() && wi::helper::toUpper(items[i].name).find(filterText) == std::string::npos)
					continue;
				visible_items++;
				wi::image::Params fx = sprites[state].params;
				fx.disableCornerRounding();
				fx.pos = XMFLOAT3(drop_x, translation.y + GetItemOffset(canvas, i), 0);
				fx.siz = XMFLOAT2(drop_width, combo_height());
				fx.color = drop_color;
				if (hovered == i)
				{
					if (combostate == COMBOSTATE_HOVER)
					{
						fx.color = sprites[FOCUS].params.color;
					}
					else if (combostate == COMBOSTATE_SELECTING)
					{
						fx.color = sprites[ACTIVE].params.color;
					}
				}
				wi::image::Draw(nullptr, fx, cmd);

				wi::font::Params fp = wi::font::Params(
					drop_x + drop_width * 0.5f,
					translation.y + combo_height() * 0.5f + GetItemOffset(canvas, i),
					wi::font::WIFONTSIZE_DEFAULT,
					wi::font::WIFALIGN_CENTER,
					wi::font::WIFALIGN_CENTER,
					font.params.color,
					font.params.shadowColor
				);
				fp.style = font.params.style;
				wi::font::Draw(items[i].name, fp, cmd);
			}
		}
	}
	void ComboBox::OnSelect(std::function<void(EventArgs args)> func)
	{
		onSelect = func;
	}
	void ComboBox::AddItem(const std::string& name, uint64_t userdata)
	{
		items.emplace_back();
		items.back().name = name;
		items.back().userdata = userdata;

		if (selected < 0 && invalid_selection_text.empty())
		{
			selected = 0;
		}
	}
	void ComboBox::RemoveItem(int index)
	{
		if (index < 0 || (size_t)index >= items.size()) {
			return;
		}

		items.erase(items.begin() + index);

		if (items.empty())
		{
			selected = -1;
		}
		else if (selected > index)
		{
			selected--;
		}
	}
	void ComboBox::ClearItems()
	{
		items.clear();

		selected = -1;
		//firstItemVisible = 0;
	}
	void ComboBox::SetMaxVisibleItemCount(int value)
	{
		maxVisibleItemCount = value;
	}
	void ComboBox::SetSelected(int index)
	{
		SetSelectedWithoutCallback(index);

		if (onSelect != nullptr)
		{
			EventArgs args;
			args.iValue = selected;
			args.sValue = GetItemText(selected);
			args.userdata = GetItemUserData(selected);
			onSelect(args);
		}
	}
	void ComboBox::SetSelectedWithoutCallback(int index)
	{
		selected = index;
	}
	void ComboBox::SetSelectedByUserdata(uint64_t userdata)
	{
		for (int i = 0; i < GetItemCount(); ++i)
		{
			if (userdata == GetItemUserData(i))
			{
				SetSelected(i);
				return;
			}
		}
	}
	void ComboBox::SetSelectedByUserdataWithoutCallback(uint64_t userdata)
	{
		for (int i = 0; i < GetItemCount(); ++i)
		{
			if (userdata == GetItemUserData(i))
			{
				SetSelectedWithoutCallback(i);
				return;
			}
		}
	}
	void ComboBox::SetItemText(int index, const std::string& text)
	{
		if (index >= 0 && index < items.size())
		{
			items[index].name = text;
		}
	}
	void ComboBox::SetItemUserdata(int index, uint64_t userdata)
	{
		if (index >= 0 && index < items.size())
		{
			items[index].userdata = userdata;
		}
	}
	void ComboBox::SetInvalidSelectionText(const std::string& text)
	{
		wi::helper::StringConvert(text, invalid_selection_text);
	}
	std::string ComboBox::GetItemText(int index) const
	{
		if (index >= 0 && index < items.size())
		{
			return items[index].name;
		}
		return "";
	}
	uint64_t ComboBox::GetItemUserData(int index) const
	{
		if (index >= 0 && index < items.size())
		{
			return items[index].userdata;
		}
		return 0;
	}
	int ComboBox::GetSelected() const
	{
		return selected;
	}
	uint64_t ComboBox::GetSelectedUserdata() const
	{
		return GetItemUserData(GetSelected());
	}
	void ComboBox::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);
		filter.SetColor(color, id);

		if (id == WIDGET_ID_COMBO_DROPDOWN)
		{
			drop_color = color;
		}
	}
	void ComboBox::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		filter.SetTheme(theme, id);

		if (id == WIDGET_ID_COMBO_DROPDOWN)
		{
			drop_color = wi::Color::fromFloat4(theme.image.color);
		}
		theme.font.Apply(selected_font.params);
	}
	void ComboBox::ExportLocalization(wi::Localization& localization) const
	{
		Widget::ExportLocalization(localization);
		if (has_flag(localization_enabled, LocalizationEnabled::Items))
		{
			wi::Localization& section = localization.GetSection(GetName()).GetSection("items");
			for (size_t i = 0; i < items.size(); ++i)
			{
				section.Add(i, items[i].name.c_str());
			}
		}
	}
	void ComboBox::ImportLocalization(const wi::Localization& localization)
	{
		Widget::ImportLocalization(localization);
		if (has_flag(localization_enabled, LocalizationEnabled::Items))
		{
			const wi::Localization* section = localization.CheckSection(GetName());
			if (section == nullptr)
				return;
			section = section->CheckSection("items");
			if (section == nullptr)
				return;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const char* localized_item_name = section->Get(i);
				if (localized_item_name != nullptr)
				{
					items[i].name = localized_item_name;
				}
			}
		}
	}






	void Window::Create(const std::string& name, WindowControls window_controls)
	{
		SetColor(wi::Color::Ghost());

		SetName(name);
		SetText(name);
		SetSize(XMFLOAT2(640, 480));

		controls = window_controls;

		for (int i = IDLE + 1; i < WIDGETSTATE_COUNT; ++i)
		{
			sprites[i].params.color = sprites[IDLE].params.color;
		}

		if (!has_flag(window_controls, WindowControls::DISABLE_TITLE_BAR))
		{
			// Add title bar controls
			if (has_flag(window_controls, WindowControls::MOVE))
			{
				// Add a grabber onto the title bar
				moveDragger.Create(name);
				moveDragger.SetLocalizationEnabled(LocalizationEnabled::None);
				moveDragger.SetShadowRadius(0);
				moveDragger.SetText(name);
				moveDragger.font.params.h_align = wi::font::WIFALIGN_LEFT;
				moveDragger.OnDrag([this](EventArgs args) {
					auto saved_parent = this->parent;
					this->Detach();
					this->Translate(XMFLOAT3(args.deltaPos.x, args.deltaPos.y, 0));
					this->AttachTo(saved_parent);
					});
				AddWidget(&moveDragger, AttachmentOptions::NONE);
				has_titlebar = true;
			}

			if (has_flag(window_controls, WindowControls::CLOSE))
			{
				// Add close button to the top left corner
				closeButton.Create(name + "_close_button");
				closeButton.SetLocalizationEnabled(LocalizationEnabled::None);
				closeButton.SetShadowRadius(0);
				closeButton.SetText("x");
				closeButton.OnClick([this](EventArgs args) {
					this->SetVisible(false);
					if (onClose)
					{
						onClose(args);
					}
					});
				closeButton.SetTooltip("Close window");
				AddWidget(&closeButton, AttachmentOptions::NONE);
				has_titlebar = true;
			}

			if (has_flag(window_controls, WindowControls::COLLAPSE))
			{
				// Add minimize button to the top left corner
				collapseButton.Create(name + "_collapse_button");
				collapseButton.SetLocalizationEnabled(LocalizationEnabled::None);
				collapseButton.SetShadowRadius(0);
				collapseButton.SetText("-");
				collapseButton.OnClick([this](EventArgs args) {
					this->SetMinimized(!this->IsMinimized());
					if (onCollapse)
					{
						onCollapse({});
					}
					});
				collapseButton.SetTooltip("Collapse/Expand window");
				AddWidget(&collapseButton, AttachmentOptions::NONE);
				has_titlebar = true;
			}

			if (!has_flag(window_controls, WindowControls::MOVE) && !name.empty())
			{
				// Simple title bar
				label.Create(name);
				label.SetLocalizationEnabled(LocalizationEnabled::None);
				label.SetShadowRadius(0);
				label.SetText(name);
				label.font.params.h_align = wi::font::WIFALIGN_LEFT;
				label.scrollbar.SetEnabled(false);
				label.SetWrapEnabled(false);
				AddWidget(&label, AttachmentOptions::NONE);
				has_titlebar = true;
			}
		}

		scrollbar_horizontal.SetVertical(false);
		scrollbar_horizontal.SetColor(wi::Color(80, 80, 80, 100), wi::gui::IDLE);
		scrollbar_horizontal.sprites_knob[ScrollBar::SCROLLBAR_INACTIVE].params.color = wi::Color(140, 140, 140, 140);
		scrollbar_horizontal.sprites_knob[ScrollBar::SCROLLBAR_HOVER].params.color = wi::Color(180, 180, 180, 180);
		scrollbar_horizontal.sprites_knob[ScrollBar::SCROLLBAR_GRABBED].params.color = wi::Color::White();
		scrollbar_horizontal.knob_inset_border = XMFLOAT2(2, 4);
		scrollbar_horizontal.SetOverScroll(0.1f);
		AddWidget(&scrollbar_horizontal);

		scrollbar_vertical.SetVertical(true);
		scrollbar_vertical.SetColor(wi::Color(80, 80, 80, 100), wi::gui::IDLE);
		scrollbar_vertical.sprites_knob[ScrollBar::SCROLLBAR_INACTIVE].params.color = wi::Color(140, 140, 140, 140);
		scrollbar_vertical.sprites_knob[ScrollBar::SCROLLBAR_HOVER].params.color = wi::Color(180, 180, 180, 180);
		scrollbar_vertical.sprites_knob[ScrollBar::SCROLLBAR_GRABBED].params.color = wi::Color::White();
		scrollbar_vertical.knob_inset_border = XMFLOAT2(4, 2);
		scrollbar_vertical.SetOverScroll(0.1f);
		AddWidget(&scrollbar_vertical);

		scrollable_area.ClearTransform();


		SetEnabled(true);
		SetVisible(true);
		SetMinimized(false);
	}
	void Window::AddWidget(Widget* widget, AttachmentOptions options)
	{
		widget->SetEnabled(this->IsEnabled());
		if (IsVisible() && !IsMinimized())
		{
			widget->SetVisible(true);
		}
		else
		{
			widget->SetVisible(false);
		}
		if (has_flag(options, AttachmentOptions::SCROLLABLE))
		{
			widget->AttachTo(&scrollable_area);
		}
		else
		{
			widget->AttachTo(this);
		}

		widgets.push_back(widget);
	}
	void Window::RemoveWidget(Widget* widget)
	{
		for (auto& x : widgets)
		{
			if (x == widget)
			{
				x = widgets.back();
				widgets.pop_back();
				break;
			}
		}
	}
	void Window::RemoveWidgets()
	{
		widgets.clear();
	}
	void Window::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Hitbox2D pointerHitbox = GetPointerHitbox();

		// Resizer updates:
		if (IsEnabled())
		{
			float vscale = IsCollapsed() ? control_size : scale.y;
			Hitbox2D lefthitbox;
			Hitbox2D righthitbox;
			Hitbox2D tophitbox;
			Hitbox2D bottomhitbox;
			Hitbox2D toplefthitbox;
			Hitbox2D toprighthitbox;
			Hitbox2D bottomrighthitbox;
			Hitbox2D bottomlefthitbox;
			if (has_flag(controls, WindowControls::RESIZE_LEFT))
			{
				lefthitbox = Hitbox2D(XMFLOAT2(translation.x - resizehitboxwidth, translation.y), XMFLOAT2(resizehitboxwidth, vscale));
			}
			if (has_flag(controls, WindowControls::RESIZE_RIGHT))
			{
				righthitbox = Hitbox2D(XMFLOAT2(translation.x + scale.x, translation.y), XMFLOAT2(resizehitboxwidth, vscale));
			}
			if (!IsCollapsed())
			{
				if (has_flag(controls, WindowControls::RESIZE_TOP))
				{
					tophitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y - resizehitboxwidth), XMFLOAT2(scale.x, resizehitboxwidth));
				}
				if (has_flag(controls, WindowControls::RESIZE_BOTTOM))
				{
					bottomhitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y + vscale), XMFLOAT2(scale.x, resizehitboxwidth));
				}
				if (has_flag(controls, WindowControls::RESIZE_TOPLEFT))
				{
					toplefthitbox = Hitbox2D(XMFLOAT2(translation.x - resizehitboxwidth, translation.y - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
				if (has_flag(controls, WindowControls::RESIZE_TOPRIGHT))
				{
					toprighthitbox = Hitbox2D(XMFLOAT2(translation.x + scale.x - resizehitboxwidth, translation.y - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
				if (has_flag(controls, WindowControls::RESIZE_BOTTOMRIGHT))
				{
					bottomrighthitbox = Hitbox2D(XMFLOAT2(translation.x + scale.x - resizehitboxwidth, translation.y + vscale - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
				if (has_flag(controls, WindowControls::RESIZE_BOTTOMLEFT))
				{
					bottomlefthitbox = Hitbox2D(XMFLOAT2(translation.x - resizehitboxwidth, translation.y + vscale - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
			}

			if (resize_state == RESIZE_STATE_NONE && wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (pointerHitbox.intersects(toplefthitbox))
				{
					resize_state = RESIZE_STATE_TOPLEFT;
					Activate();
				}
				else if (pointerHitbox.intersects(toprighthitbox))
				{
					resize_state = RESIZE_STATE_TOPRIGHT;
					Activate();
				}
				else if (pointerHitbox.intersects(bottomrighthitbox))
				{
					resize_state = RESIZE_STATE_BOTTOMRIGHT;
					Activate();
				}
				else if (pointerHitbox.intersects(bottomlefthitbox))
				{
					resize_state = RESIZE_STATE_BOTTOMLEFT;
					Activate();
				}
				else if (pointerHitbox.intersects(lefthitbox))
				{
					resize_state = RESIZE_STATE_LEFT;
					Activate();
				}
				else if (pointerHitbox.intersects(righthitbox))
				{
					resize_state = RESIZE_STATE_RIGHT;
					Activate();
				}
				else if (pointerHitbox.intersects(tophitbox))
				{
					resize_state = RESIZE_STATE_TOP;
					Activate();
				}
				else if (pointerHitbox.intersects(bottomhitbox))
				{
					resize_state = RESIZE_STATE_BOTTOM;
					Activate();
				}
				resize_begin = pointerHitbox.pos;
			}
			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (resize_state != RESIZE_STATE_NONE)
				{
					auto saved_parent = this->parent;
					this->Detach();
					float deltaX = pointerHitbox.pos.x - resize_begin.x;
					float deltaY = pointerHitbox.pos.y - resize_begin.y;

					switch (resize_state)
					{
					case wi::gui::Window::RESIZE_STATE_LEFT:
						this->Translate(XMFLOAT3(deltaX, 0, 0));
						this->Scale(XMFLOAT3((scale.x - deltaX) / scale.x, 1, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_TOP:
						this->Translate(XMFLOAT3(0, deltaY, 0));
						this->Scale(XMFLOAT3(1, (scale.y - deltaY) / scale.y, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_RIGHT:
						this->Scale(XMFLOAT3((scale.x + deltaX) / scale.x, 1, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_BOTTOM:
						this->Scale(XMFLOAT3(1, (scale.y + deltaY) / scale.y, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_TOPLEFT:
						this->Translate(XMFLOAT3(deltaX, deltaY, 0));
						this->Scale(XMFLOAT3((scale.x - deltaX) / scale.x, (scale.y - deltaY) / scale.y, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_TOPRIGHT:
						this->Translate(XMFLOAT3(0, deltaY, 0));
						this->Scale(XMFLOAT3((scale.x + deltaX) / scale.x, (scale.y - deltaY) / scale.y, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_BOTTOMRIGHT:
						this->Scale(XMFLOAT3((scale.x + deltaX) / scale.x, (scale.y + deltaY) / scale.y, 1));
						break;
					case wi::gui::Window::RESIZE_STATE_BOTTOMLEFT:
						this->Translate(XMFLOAT3(deltaX, 0, 0));
						this->Scale(XMFLOAT3((scale.x - deltaX) / scale.x, (scale.y + deltaY) / scale.y, 1));
						break;
					default:
						break;
					}

					this->scale_local = wi::math::Max(this->scale_local, XMFLOAT3(control_size * 3, control_size * 2, 1)); // don't allow resize to negative or too small
					this->AttachTo(saved_parent);
					resize_begin = pointerHitbox.pos;
				}
			}
			else
			{
				resize_state = RESIZE_STATE_NONE;
			}

			if (
				resize_state != RESIZE_STATE_NONE ||
				pointerHitbox.intersects(toplefthitbox) ||
				pointerHitbox.intersects(toprighthitbox) ||
				pointerHitbox.intersects(bottomlefthitbox) ||
				pointerHitbox.intersects(bottomrighthitbox) ||
				pointerHitbox.intersects(lefthitbox) ||
				pointerHitbox.intersects(righthitbox) ||
				pointerHitbox.intersects(tophitbox) ||
				pointerHitbox.intersects(bottomhitbox)
				)
			{
				resize_blink_timer += dt;
			}
			else
			{
				resize_blink_timer = 0;
			}
		}

		// Corner rounding update for control widgets:
		for (int i = 0; i < arraysize(moveDragger.sprites); ++i)
		{
			moveDragger.sprites[i].params.disableCornerRounding();
			label.sprites[i].params.disableCornerRounding();
			collapseButton.sprites[i].params.disableCornerRounding();
			closeButton.sprites[i].params.disableCornerRounding();
			scrollbar_horizontal.sprites[i].params.disableCornerRounding();
			scrollbar_vertical.sprites[i].params.disableCornerRounding();

			if (sprites[state].params.isCornerRoundingEnabled())
			{
				// Left side:
				if (collapseButton.parent)
				{
					collapseButton.sprites[i].params.enableCornerRounding();
					collapseButton.sprites[i].params.corners_rounding[0].radius = sprites[state].params.corners_rounding[0].radius;
					if (IsCollapsed())
					{
						collapseButton.sprites[i].params.corners_rounding[2].radius = sprites[state].params.corners_rounding[2].radius;
					}
					else
					{
						collapseButton.sprites[i].params.corners_rounding[2].radius = 0;
					}
				}
				else if (moveDragger.parent)
				{
					moveDragger.sprites[i].params.enableCornerRounding();
					moveDragger.sprites[i].params.corners_rounding[0].radius = sprites[state].params.corners_rounding[0].radius;
					if (IsCollapsed())
					{
						moveDragger.sprites[i].params.corners_rounding[2].radius = sprites[state].params.corners_rounding[2].radius;
					}
					else
					{
						moveDragger.sprites[i].params.corners_rounding[2].radius = 0;
					}
				}
				else if (label.parent)
				{
					label.sprites[i].params.enableCornerRounding();
					label.sprites[i].params.corners_rounding[0].radius = sprites[state].params.corners_rounding[0].radius;
					if (IsCollapsed())
					{
						label.sprites[i].params.corners_rounding[2].radius = sprites[state].params.corners_rounding[2].radius;
					}
					else
					{
						label.sprites[i].params.corners_rounding[2].radius = 0;
					}
				}

				// Right side:
				if (closeButton.parent)
				{
					closeButton.sprites[i].params.enableCornerRounding();
					closeButton.sprites[i].params.corners_rounding[1].radius = sprites[state].params.corners_rounding[1].radius;
					if (IsCollapsed())
					{
						closeButton.sprites[i].params.corners_rounding[3].radius = sprites[state].params.corners_rounding[3].radius;
					}
					else
					{
						closeButton.sprites[i].params.corners_rounding[3].radius = 0;
					}
				}
				else if (moveDragger.parent)
				{
					moveDragger.sprites[i].params.enableCornerRounding();
					moveDragger.sprites[i].params.corners_rounding[1].radius = sprites[state].params.corners_rounding[1].radius;
					if (IsCollapsed())
					{
						moveDragger.sprites[i].params.corners_rounding[3].radius = sprites[state].params.corners_rounding[3].radius;
					}
					else
					{
						moveDragger.sprites[i].params.corners_rounding[3].radius = 0;
					}
				}
				else if (label.parent)
				{
					label.sprites[i].params.enableCornerRounding();
					label.sprites[i].params.corners_rounding[1].radius = sprites[state].params.corners_rounding[1].radius;
					if (IsCollapsed())
					{
						label.sprites[i].params.corners_rounding[3].radius = sprites[state].params.corners_rounding[3].radius;
					}
					else
					{
						label.sprites[i].params.corners_rounding[3].radius = 0;
					}
				}

				scrollbar_horizontal.sprites[i].params.enableCornerRounding();
				scrollbar_horizontal.sprites[i].params.corners_rounding[3].radius = sprites[state].params.corners_rounding[3].radius;
				scrollbar_vertical.sprites[i].params.enableCornerRounding();
				scrollbar_vertical.sprites[i].params.corners_rounding[3].radius = sprites[state].params.corners_rounding[3].radius;
			}
		}

		moveDragger.force_disable = force_disable;
		scrollbar_horizontal.force_disable = force_disable;
		scrollbar_vertical.force_disable = force_disable;

		moveDragger.Update(canvas, dt);

		// Don't allow moving outside of screen:
		if (parent == nullptr)
		{
			translation_local.x = wi::math::Clamp(translation_local.x, 0, canvas.GetLogicalWidth() - scale_local.x);
			translation_local.y = wi::math::Clamp(translation_local.y, 0, canvas.GetLogicalHeight() - control_size);
			SetDirty();
		}

		Widget::Update(canvas, dt);

		ResizeLayout();

		uint32_t priority = 0;

		scrollable_area.scissorRect = scissorRect;

		scrollable_area.Detach();
		scrollable_area.ClearTransform();
		scrollable_area.Translate(translation);

		float scroll_length_horizontal = 0;
		float scroll_length_vertical = 0;
		for (auto& widget : widgets)
		{
			if (!widget->IsVisible())
				continue;
			if (widget->parent == &scrollable_area)
			{
				XMFLOAT2 size = widget->GetSize();
				scroll_length_horizontal = std::max(scroll_length_horizontal, widget->translation_local.x + size.x);
				scroll_length_vertical = std::max(scroll_length_vertical, widget->translation_local.y + size.y);
			}
		}

		if (has_flag(controls, WindowControls::FIT_ALL_WIDGETS_VERTICAL))
		{
			if (!IsCollapsed())
			{
				// it will be dynamically sized to fit all widgets:
				auto saved_parent = this->parent;
				this->Detach();
				scale_local.y = control_size + 1 + scroll_length_vertical + 4; // some padding at the bottom
				this->AttachTo(saved_parent);
			}
		}
		else
		{
			// Compute scrollable area:
			if (scrollbar_horizontal.parent != nullptr || scrollbar_vertical.parent != nullptr)
			{
				scrollbar_horizontal.SetListLength(scroll_length_horizontal);
				scrollbar_vertical.SetListLength(scroll_length_vertical);
				scrollbar_horizontal.Update(canvas, 0);
				scrollbar_vertical.Update(canvas, 0);
				scrollable_area.Translate(XMFLOAT3(scrollbar_horizontal.GetOffset(), 1 + scrollbar_vertical.GetOffset(), 0));
				scrollable_area.scissorRect.left += 1;
				if (scrollbar_horizontal.parent != nullptr && scrollbar_horizontal.IsScrollbarRequired())
				{
					scrollable_area.scissorRect.bottom -= (int32_t)control_size + 1;
				}
				if (scrollbar_vertical.parent != nullptr && scrollbar_vertical.IsScrollbarRequired())
				{
					scrollable_area.scissorRect.right -= (int32_t)control_size + 1;
				}
				scrollable_area.active_area.pos.x = float(scrollable_area.scissorRect.left);
				scrollable_area.active_area.pos.y = float(scrollable_area.scissorRect.top);
				scrollable_area.active_area.siz.x = float(scrollable_area.scissorRect.right) - float(scrollable_area.scissorRect.left);
				scrollable_area.active_area.siz.y = float(scrollable_area.scissorRect.bottom) - float(scrollable_area.scissorRect.top);
			}
		}

		if (has_titlebar)
		{
			scrollable_area.Translate(XMFLOAT3(0, control_size, 0));
			scrollable_area.scissorRect.top += (int32_t)control_size;
		}

		scrollable_area.AttachTo(this);

		for (size_t i = 0; i < widgets.size(); ++i)
		{
			Widget* widget = widgets[i]; // re index in loop, because widgets can be realloced while updating!
			widget->force_disable = force_disable;
			widget->Update(canvas, dt);
			widget->force_disable = false;
			if (widget->GetState() > FOCUS)
			{
				force_disable = true;
			}

			if (widget->priority_change)
			{
				widget->priority_change = false;
				widget->priority = priority++;
			}
			else
			{
				widget->priority = ~0u;
			}
		}
		force_disable = false;

		if (priority > 0)
		{
			// Sort only if there are priority changes
			//	Use std::stable_sort instead of std::sort to preserve UI element order with equal priorities
			std::stable_sort(widgets.begin(), widgets.end(), [](const Widget* a, const Widget* b) {
				return a->priority < b->priority;
			});
		}

		if (!IsMinimized() && IsVisible())
		{
			float scroll = wi::input::GetPointer().z * 20;
			if (scroll && scroll_allowed && scrollbar_vertical.IsScrollbarRequired() && pointerHitbox.intersects(hitBox)) // when window is in focus, but other widgets aren't
			{
				scroll_allowed = false;
				// This is outside scrollbar code, because it can also be scrolled if parent widget is only in focus
				scrollbar_vertical.Scroll(scroll);
			}
		}

		if (IsMinimized())
		{
			hitBox.siz.y = control_size;
		}

		if (IsEnabled() && !IsMinimized() && dt > 0)
		{
			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}
			if (state == ACTIVE)
			{
				Deactivate();
			}


			bool clicked = false;
			if (pointerHitbox.intersects(hitBox))
			{
				if (state == IDLE)
				{
					state = FOCUS;
				}
			}

			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (state == FOCUS)
				{
					// activate
					clicked = true;
				}
			}

			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (state == DEACTIVATING)
				{
					// Keep pressed until mouse is released
					Activate();
				}
			}

			if (clicked)
			{
				Activate();
			}
		}
		else
		{
			state = IDLE;
		}

		if (state == IDLE && resize_blink_timer > 0)
			state = FOCUS;
	}
	void Window::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}

		GetDevice()->EventBegin(name.c_str(), cmd);

		wi::Color color = GetColor();

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y += shadow * 2;
			fx.color = shadow_color;
			if (IsMinimized())
			{
				fx.siz.y = control_size + shadow * 2;
			}
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		// resize indicator:
		{
			// hitboxes are recomputed because window transform might have changed since update!!
			float vscale = IsCollapsed() ? control_size : scale.y;
			Hitbox2D lefthitbox;
			Hitbox2D righthitbox;
			Hitbox2D tophitbox;
			Hitbox2D bottomhitbox;
			Hitbox2D toplefthitbox;
			Hitbox2D toprighthitbox;
			Hitbox2D bottomrighthitbox;
			Hitbox2D bottomlefthitbox;
			if (has_flag(controls, WindowControls::RESIZE_LEFT))
			{
				lefthitbox = Hitbox2D(XMFLOAT2(translation.x - resizehitboxwidth, translation.y), XMFLOAT2(resizehitboxwidth, vscale));
			}
			if (has_flag(controls, WindowControls::RESIZE_RIGHT))
			{
				righthitbox = Hitbox2D(XMFLOAT2(translation.x + scale.x, translation.y), XMFLOAT2(resizehitboxwidth, vscale));
			}
			if (!IsCollapsed())
			{
				if (has_flag(controls, WindowControls::RESIZE_TOP))
				{
					tophitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y - resizehitboxwidth), XMFLOAT2(scale.x, resizehitboxwidth));
				}
				if (has_flag(controls, WindowControls::RESIZE_BOTTOM))
				{
					bottomhitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y + vscale), XMFLOAT2(scale.x, resizehitboxwidth));
				}
				if (has_flag(controls, WindowControls::RESIZE_TOPLEFT))
				{
					toplefthitbox = Hitbox2D(XMFLOAT2(translation.x - resizehitboxwidth, translation.y - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
				if (has_flag(controls, WindowControls::RESIZE_TOPRIGHT))
				{
					toprighthitbox = Hitbox2D(XMFLOAT2(translation.x + scale.x - resizehitboxwidth, translation.y - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
				if (has_flag(controls, WindowControls::RESIZE_BOTTOMRIGHT))
				{
					bottomrighthitbox = Hitbox2D(XMFLOAT2(translation.x + scale.x - resizehitboxwidth, translation.y + vscale - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
				if (has_flag(controls, WindowControls::RESIZE_BOTTOMLEFT))
				{
					bottomlefthitbox = Hitbox2D(XMFLOAT2(translation.x - resizehitboxwidth, translation.y + vscale - resizehitboxwidth), XMFLOAT2(resizehitboxwidth * 2, resizehitboxwidth * 2));
				}
			}

			const Hitbox2D pointerHitbox = GetPointerHitbox();

			wi::image::Params fx = sprites[state].params;
			fx.blendFlag = wi::enums::BLENDMODE_ALPHA;
			fx.pos.x -= resizehitboxwidth;
			fx.pos.y -= resizehitboxwidth;
			fx.siz.x += resizehitboxwidth * 2;
			fx.siz.y += resizehitboxwidth * 2;
			fx.color = resize_state == RESIZE_STATE_NONE ? sprites[FOCUS].params.color : sprites[ACTIVE].params.color;
			if (IsMinimized())
			{
				fx.siz.y = control_size + resizehitboxwidth * 2;
			}
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += resizehitboxwidth;
					}
				}
			}
			//fx.border_soften = 0.01f;

			if (resize_state == RESIZE_STATE_TOPLEFT || pointerHitbox.intersects(toplefthitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.03f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.025f, std::abs(std::sin(resize_blink_timer * 4)));
				XMStoreFloat2(&fx.angular_softness_direction, XMVector2Normalize(XMVectorSet(-1, -1, 0, 0)));
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NWSE);
			}
			else if (resize_state == RESIZE_STATE_TOPRIGHT || pointerHitbox.intersects(toprighthitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.03f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.025f, std::abs(std::sin(resize_blink_timer * 4)));
				XMStoreFloat2(&fx.angular_softness_direction, XMVector2Normalize(XMVectorSet(1, -1, 0, 0)));
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NESW);
			}
			else if (resize_state == RESIZE_STATE_BOTTOMRIGHT || pointerHitbox.intersects(bottomrighthitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.03f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.025f, std::abs(std::sin(resize_blink_timer * 4)));
				XMStoreFloat2(&fx.angular_softness_direction, XMVector2Normalize(XMVectorSet(1, 1, 0, 0)));
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NWSE);
			}
			else if (resize_state == RESIZE_STATE_BOTTOMLEFT || pointerHitbox.intersects(bottomlefthitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.03f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.025f, std::abs(std::sin(resize_blink_timer * 4)));
				XMStoreFloat2(&fx.angular_softness_direction, XMVector2Normalize(XMVectorSet(-1, 1, 0, 0)));
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NESW);
			}
			else if (resize_state == RESIZE_STATE_LEFT || pointerHitbox.intersects(lefthitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.25f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.24f, std::abs(std::sin(resize_blink_timer * 4)));
				fx.angular_softness_direction = XMFLOAT2(-1, 0);
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_EW);
			}
			else if (resize_state == RESIZE_STATE_RIGHT || pointerHitbox.intersects(righthitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.25f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.24f, std::abs(std::sin(resize_blink_timer * 4)));
				fx.angular_softness_direction = XMFLOAT2(1, 0);
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_EW);
			}
			else if (resize_state == RESIZE_STATE_TOP || pointerHitbox.intersects(tophitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.25f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.24f, std::abs(std::sin(resize_blink_timer * 4)));
				fx.angular_softness_direction = XMFLOAT2(0, -1);
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NS);
			}
			else if (resize_state == RESIZE_STATE_BOTTOM || pointerHitbox.intersects(bottomhitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.25f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.24f, std::abs(std::sin(resize_blink_timer * 4)));
				fx.angular_softness_direction = XMFLOAT2(0, 1);
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NS);
			}
		}

		// base:
		if (!IsCollapsed())
		{
			wi::image::Params params = sprites[IDLE].params;
			const wi::Resource& res = sprites[IDLE].textureResource;
			if (res.IsValid())
			{
				params.sampleFlag = wi::image::SAMPLEMODE_WRAP;
				const Texture& tex = res.GetTexture();
				const float widget_aspect = scale_local.x / scale_local.y;
				const float image_aspect = float(tex.desc.width) / float(tex.desc.height);
				if (widget_aspect > image_aspect)
				{
					// display aspect is wider than image:
					params.texMulAdd.y *= image_aspect / widget_aspect;
				}
				else
				{
					// image aspect is wider or equal to display
					params.texMulAdd.x *= widget_aspect / image_aspect;
					if (right_aligned_image)
					{
						params.texMulAdd.z = 1.0f - params.texMulAdd.x;
					}
				}
			}
			wi::image::Draw(sprites[IDLE].GetTexture(), params, cmd);

			if (background_overlay.IsValid())
			{
				params.blendFlag = wi::enums::BLENDMODE_ADDITIVE;
				params.color.w = 0;
				params.setBackgroundMap(&background_overlay);
				wi::image::Draw(nullptr, params, cmd);
			}
		}

		for (size_t i = 0; i < widgets.size(); ++i)
		{
			const Widget* widget = widgets[widgets.size() - i - 1];
			if (widget->parent == nullptr)
			{
				ApplyScissor(canvas, scissorRect, cmd);
			}
			else
			{
				ApplyScissor(canvas, widget->parent->scissorRect, cmd);
			}
			widget->Render(canvas, cmd);
		}

		//Rect scissorRect;
		//scissorRect.bottom = (int32_t)(canvas.GetPhysicalHeight());
		//scissorRect.left = (int32_t)(0);
		//scissorRect.right = (int32_t)(canvas.GetPhysicalWidth());
		//scissorRect.top = (int32_t)(0);
		//GraphicsDevice* device = wi::graphics::GetDevice();
		//device->BindScissorRects(1, &scissorRect, cmd);
		//wi::image::Draw(nullptr, wi::image::Params(scrollable_area.active_area.pos.x, scrollable_area.active_area.pos.y, scrollable_area.active_area.siz.x, scrollable_area.active_area.siz.y, wi::Color(255,0,255,100)), cmd);
		//Hitbox2D p = scrollable_area.GetPointerHitbox();
		//wi::image::Draw(nullptr, wi::image::Params(p.pos.x, p.pos.y, p.siz.x * 10, p.siz.y * 10, wi::Color(255,0,0,100)), cmd);
		//if (!IsCollapsed())
		//{
		//	wi::image::Draw(nullptr, wi::image::Params(scrollable_area.translation.x, scrollable_area.translation.y, scale.x, 10, wi::Color(255,0,255,100)), cmd);
		//}

		GetDevice()->EventEnd(cmd);
	}
	void Window::RenderTooltip(const wi::Canvas& canvas, wi::graphics::CommandList cmd) const
	{
		// Window base tooltip is not rendered
		for (auto& x : widgets)
		{
			x->RenderTooltip(canvas, cmd);
		}
	}
	void Window::SetVisible(bool value)
	{
		Widget::SetVisible(value);
		bool minimized = IsMinimized();
		for (auto& x : widgets)
		{
			if (
				x == &closeButton ||
				x == &collapseButton ||
				x == &moveDragger ||
				x == &label
				)
			{
				x->SetVisible(value);
			}
			else
			{
				x->SetVisible(!minimized);
			}
		}
	}
	void Window::SetEnabled(bool value)
	{
		Widget::SetEnabled(value);
		for (auto& x : widgets)
		{
			if (x == &moveDragger)
				continue;
			if (x == &collapseButton)
				continue;
			if (x == &closeButton)
				continue;
			if (x == &scrollbar_horizontal)
				continue;
			if (x == &scrollbar_vertical)
				continue;
			x->SetEnabled(value);
		}
	}
	void Window::SetCollapsed(bool value)
	{
		SetMinimized(value);
	}
	bool Window::IsCollapsed() const
	{
		return IsMinimized();
	}
	void Window::SetMinimized(bool value)
	{
		minimized = value;

		for (auto& x : widgets)
		{
			if (
				x == &closeButton ||
				x == &collapseButton ||
				x == &moveDragger ||
				x == &label
				)
			{
				continue;
			}
			x->SetVisible(!value);
		}

		scrollable_area.SetVisible(!value);

		if (IsMinimized())
		{
			collapseButton.SetText("»");
			collapseButton.font.params.rotation = XM_PIDIV2;
		}
		else
		{
			collapseButton.SetText("-");
			collapseButton.font.params.rotation = 0;
		}
	}
	bool Window::IsMinimized() const
	{
		return minimized;
	}
	void Window::SetControlSize(float value)
	{
		control_size = value;
	}
	float Window::GetControlSize() const
	{
		return control_size;
	}
	XMFLOAT2 Window::GetSize() const
	{
		XMFLOAT2 size = Widget::GetSize();
		if (IsCollapsed())
		{
			return XMFLOAT2(size.x, control_size);
		}
		return size;
	}
	XMFLOAT2 Window::GetWidgetAreaSize() const
	{
		XMFLOAT2 size = GetSize();
		if (scrollbar_horizontal.IsScrollbarRequired())
		{
			size.y -= control_size;
		}
		if (scrollbar_vertical.IsScrollbarRequired())
		{
			size.x -= control_size;
		}
		return size;
	}
	void Window::OnClose(std::function<void(EventArgs args)> func)
	{
		onClose = func;
	}
	void Window::OnCollapse(std::function<void(EventArgs args)> func)
	{
		onCollapse = func;
	}
	void Window::OnResize(std::function<void()> func)
	{
		onResize = func;
	}
	void Window::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);
		for (auto& widget : widgets)
		{
			widget->SetColor(color, id);
		}

		if (id == WIDGET_ID_WINDOW_BASE)
		{
			sprites[IDLE].params.color = color;
		}
	}
	void Window::SetImage(wi::Resource resource, int id)
	{
		Widget::SetImage(resource, id);
		for (auto& widget : widgets)
		{
			widget->SetImage(resource, id);
		}

		if (id == WIDGET_ID_WINDOW_BASE)
		{
			sprites[IDLE].textureResource = resource;
		}
	}
	void Window::SetShadowColor(wi::Color color)
	{
		Widget::SetShadowColor(color);
		for (auto& widget : widgets)
		{
			widget->SetShadowColor(color);
		}
	}
	void Window::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		for (auto& widget : widgets)
		{
			widget->SetTheme(theme, id);
		}
	}
	void Window::ResizeLayout()
	{
		Widget::ResizeLayout();
		layout.reset(*this);
		for (auto& widget : widgets)
		{
			widget->ResizeLayout();
		}

		if (moveDragger.parent != nullptr)
		{
			moveDragger.Detach();
			float rem = 0;
			if (closeButton.parent != nullptr)
			{
				rem++;
			}
			if (collapseButton.parent != nullptr)
			{
				rem++;
			}
			moveDragger.SetSize(XMFLOAT2(scale.x - control_size * rem, control_size));
			float offset = 0;
			if (collapseButton.parent != nullptr)
			{
				offset++;
			}
			moveDragger.SetPos(XMFLOAT2(translation.x + control_size * offset, translation.y));
			moveDragger.AttachTo(this);
		}
		if (closeButton.parent != nullptr)
		{
			closeButton.Detach();
			closeButton.SetSize(XMFLOAT2(control_size, control_size));
			closeButton.SetPos(XMFLOAT2(translation.x + scale.x - control_size, translation.y));
			closeButton.AttachTo(this);
		}
		if (collapseButton.parent != nullptr)
		{
			collapseButton.Detach();
			collapseButton.SetSize(XMFLOAT2(control_size, control_size));
			collapseButton.SetPos(XMFLOAT2(translation.x, translation.y));
			collapseButton.AttachTo(this);
		}
		if (label.parent != nullptr)
		{
			label.font.params = font.params;
			label.Detach();
			XMFLOAT2 label_size = XMFLOAT2(scale.x, control_size);
			XMFLOAT2 label_pos = XMFLOAT2(translation.x, translation.y);
			if (closeButton.parent != nullptr)
			{
				label_size.x -= control_size;
			}
			if (collapseButton.parent != nullptr)
			{
				label_size.x -= control_size;
				label_pos.x += control_size;
			}
			label.SetSize(label_size);
			label.SetPos(label_pos);
			label.AttachTo(this);
		}
		if (scrollbar_horizontal.parent != nullptr)
		{
			scrollbar_horizontal.Detach();
			scrollbar_horizontal.SetSize(XMFLOAT2(GetWidgetAreaSize().x - control_size, control_size));
			scrollbar_horizontal.SetPos(XMFLOAT2(translation.x + control_size, translation.y + scale.y - control_size));
			scrollbar_horizontal.AttachTo(this);
			scrollbar_horizontal.SetSafeArea(control_size * 2);
		}
		if (scrollbar_vertical.parent != nullptr)
		{
			scrollbar_vertical.Detach();
			scrollbar_vertical.SetSize(XMFLOAT2(control_size, GetWidgetAreaSize().y - control_size));
			scrollbar_vertical.SetPos(XMFLOAT2(translation.x + scale.x - control_size, translation.y + control_size));
			scrollbar_vertical.AttachTo(this);
		}

		if (onResize)
		{
			onResize();
		}
	}
	void Window::ExportLocalization(wi::Localization& localization) const
	{
		Widget::ExportLocalization(localization);
		if (has_flag(localization_enabled, LocalizationEnabled::Children))
		{
			wi::Localization& section = localization.GetSection(GetName());
			for (auto& widget : widgets)
			{
				widget->ExportLocalization(section);
			}
		}
	}
	void Window::ImportLocalization(const wi::Localization& localization)
	{
		Widget::ImportLocalization(localization);
		if (has_flag(localization_enabled, LocalizationEnabled::Children))
		{
			const wi::Localization* section = localization.CheckSection(GetName());
			if (section == nullptr)
				return;
			for (auto& widget : widgets)
			{
				widget->ImportLocalization(*section);
			}
		}
		label.SetText(GetText());
		moveDragger.SetText(GetText());
	}






	struct rgb {
		float r;       // a fraction between 0 and 1
		float g;       // a fraction between 0 and 1
		float b;       // a fraction between 0 and 1
	};
	struct hsv {
		float h;       // angle in degrees
		float s;       // a fraction between 0 and 1
		float v;       // a fraction between 0 and 1
	};
	hsv rgb2hsv(rgb in)
	{
		hsv         out;
		float		min, max, delta;

		min = in.r < in.g ? in.r : in.g;
		min = min < in.b ? min : in.b;

		max = in.r > in.g ? in.r : in.g;
		max = max > in.b ? max : in.b;

		out.v = max;                                // v
		delta = max - min;
		if (delta < 0.00001f)
		{
			out.s = 0;
			out.h = 0; // undefined, maybe nan?
			return out;
		}
		if (max > 0.0f) { // NOTE: if Max is == 0, this divide would cause a crash
			out.s = (delta / max);                  // s
		}
		else {
			// if max is 0, then r = g = b = 0
			// s = 0, h is undefined
			out.s = 0.0f;
			out.h = NAN;                            // its now undefined
			return out;
		}
		if (in.r >= max)                           // > is bogus, just keeps compilor happy
			out.h = (in.g - in.b) / delta;        // between yellow & magenta
		else
			if (in.g >= max)
				out.h = 2.0f + (in.b - in.r) / delta;  // between cyan & yellow
			else
				out.h = 4.0f + (in.r - in.g) / delta;  // between magenta & cyan

		out.h *= 60.0f;                              // degrees

		if (out.h < 0.0f)
			out.h += 360.0f;

		return out;
	}
	rgb hsv2rgb(hsv in)
	{
		float		hh, p, q, t, ff;
		long        i;
		rgb         out;

		if (in.s <= 0.0f) {       // < is bogus, just shuts up warnings
			out.r = in.v;
			out.g = in.v;
			out.b = in.v;
			return out;
		}
		hh = in.h;
		if (hh >= 360.0f) hh = 0.0f;
		hh /= 60.0f;
		i = (long)hh;
		ff = hh - i;
		p = in.v * (1.0f - in.s);
		q = in.v * (1.0f - (in.s * ff));
		t = in.v * (1.0f - (in.s * (1.0f - ff)));

		switch (i) {
		case 0:
			out.r = in.v;
			out.g = t;
			out.b = p;
			break;
		case 1:
			out.r = q;
			out.g = in.v;
			out.b = p;
			break;
		case 2:
			out.r = p;
			out.g = in.v;
			out.b = t;
			break;

		case 3:
			out.r = p;
			out.g = q;
			out.b = in.v;
			break;
		case 4:
			out.r = t;
			out.g = p;
			out.b = in.v;
			break;
		case 5:
		default:
			out.r = in.v;
			out.g = p;
			out.b = q;
			break;
		}
		return out;
	}

	static const float cp_width = 300;
	static const float cp_height = 260;
	void ColorPicker::Create(const std::string& name, WindowControls window_controls)
	{
		Window::Create(name, window_controls);

		SetSize(XMFLOAT2(cp_width, cp_height));
		SetColor(wi::Color(100, 100, 100, 100));

		float x = 250;
		float y = 80;
		float step = 20;

		text_R.Create("R");
		text_R.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_R.SetPos(XMFLOAT2(x, y += step));
		text_R.SetSize(XMFLOAT2(40, 18));
		text_R.SetText("");
		text_R.SetTooltip("Enter value for RED channel (0-255)");
		text_R.SetDescription("R: ");
		text_R.OnInputAccepted([this](EventArgs args) {
			wi::Color color = GetPickColor();
			color.setR((uint8_t)args.iValue);
			SetPickColor(color);
			FireEvents();
			});
		AddWidget(&text_R);

		text_G.Create("G");
		text_G.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_G.SetPos(XMFLOAT2(x, y += step));
		text_G.SetSize(XMFLOAT2(40, 18));
		text_G.SetText("");
		text_G.SetTooltip("Enter value for GREEN channel (0-255)");
		text_G.SetDescription("G: ");
		text_G.OnInputAccepted([this](EventArgs args) {
			wi::Color color = GetPickColor();
			color.setG((uint8_t)args.iValue);
			SetPickColor(color);
			FireEvents();
			});
		AddWidget(&text_G);

		text_B.Create("B");
		text_B.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_B.SetPos(XMFLOAT2(x, y += step));
		text_B.SetSize(XMFLOAT2(40, 18));
		text_B.SetText("");
		text_B.SetTooltip("Enter value for BLUE channel (0-255)");
		text_B.SetDescription("B: ");
		text_B.OnInputAccepted([this](EventArgs args) {
			wi::Color color = GetPickColor();
			color.setB((uint8_t)args.iValue);
			SetPickColor(color);
			FireEvents();
			});
		AddWidget(&text_B);


		text_H.Create("H");
		text_H.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_H.SetPos(XMFLOAT2(x, y += step));
		text_H.SetSize(XMFLOAT2(40, 18));
		text_H.SetText("");
		text_H.SetTooltip("Enter value for HUE channel (0-360)");
		text_H.SetDescription("H: ");
		text_H.OnInputAccepted([this](EventArgs args) {
			hue = wi::math::Clamp(args.fValue, 0, 360.0f);
			FireEvents();
			});
		AddWidget(&text_H);

		text_S.Create("S");
		text_S.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_S.SetPos(XMFLOAT2(x, y += step));
		text_S.SetSize(XMFLOAT2(40, 18));
		text_S.SetText("");
		text_S.SetTooltip("Enter value for SATURATION channel (0-100)");
		text_S.SetDescription("S: ");
		text_S.OnInputAccepted([this](EventArgs args) {
			saturation = wi::math::Clamp(args.fValue / 100.0f, 0, 1);
			FireEvents();
			});
		AddWidget(&text_S);

		text_V.Create("V");
		text_V.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_V.SetPos(XMFLOAT2(x, y += step));
		text_V.SetSize(XMFLOAT2(40, 18));
		text_V.SetText("");
		text_V.SetTooltip("Enter value for LUMINANCE channel (0-100)");
		text_V.SetDescription("V: ");
		text_V.OnInputAccepted([this](EventArgs args) {
			luminance = wi::math::Clamp(args.fValue / 100.0f, 0, 1);
			FireEvents();
			});
		AddWidget(&text_V);

		text_hex.Create("Hex");
		text_hex.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		text_hex.SetPos(XMFLOAT2(x, y += step));
		text_hex.SetSize(XMFLOAT2(80, 18));
		text_hex.SetText("");
		text_hex.SetTooltip("Enter RGBA hex value");
		text_hex.SetDescription("#");
		text_hex.font_description.params.scaling = 1.2f;
		text_hex.OnInputAccepted([this](EventArgs args) {
			wi::Color color(args.sValue.c_str());
			SetPickColor(color);
			FireEvents();
			});
		AddWidget(&text_hex);

		alphaSlider.Create(0, 255, 255, 255, "");
		alphaSlider.SetLocalizationEnabled(LocalizationEnabled::Tooltip);
		alphaSlider.SetPos(XMFLOAT2(20, y));
		alphaSlider.SetSize(XMFLOAT2(150, 18));
		alphaSlider.SetText("A: ");
		alphaSlider.SetTooltip("Value for ALPHA - TRANSPARENCY channel (0-255)");
		alphaSlider.OnSlide([this](EventArgs args) {
			FireEvents();
			});
		AddWidget(&alphaSlider);
	}
	static const float colorpicker_radius_triangle = 68;
	static const float colorpicker_radius = 75;
	static const float colorpicker_width = 22;
	void ColorPicker::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Window::Update(canvas, dt);

		if (IsEnabled() && dt > 0)
		{

			if (state == DEACTIVATING)
			{
				state = IDLE;
			}

			float sca = std::min(scale.x / cp_width, scale.y / cp_height);

			XMMATRIX W =
				XMMatrixScaling(sca, sca, 1) *
				XMMatrixTranslation(translation.x + scale.x * 0.4f, translation.y + scale.y * 0.5f, 0)
				;

			XMFLOAT2 center = XMFLOAT2(translation.x + scale.x * 0.4f, translation.y + scale.y * 0.5f);
			XMFLOAT2 pointer = GetPointerHitbox().pos;
			float distance = wi::math::Distance(center, pointer);
			bool hover_hue = (distance > colorpicker_radius * sca) && (distance < (colorpicker_radius + colorpicker_width)* sca);

			float distTri = 0;
			XMFLOAT4 A, B, C;
			wi::math::ConstructTriangleEquilateral(colorpicker_radius_triangle, A, B, C);
			XMVECTOR _A = XMLoadFloat4(&A);
			XMVECTOR _B = XMLoadFloat4(&B);
			XMVECTOR _C = XMLoadFloat4(&C);
			XMMATRIX _triTransform = XMMatrixRotationZ(-hue / 360.0f * XM_2PI) * W;
			_A = XMVector4Transform(_A, _triTransform);
			_B = XMVector4Transform(_B, _triTransform);
			_C = XMVector4Transform(_C, _triTransform);
			XMVECTOR O = XMVectorSet(pointer.x, pointer.y, 0, 0);
			XMVECTOR D = XMVectorSet(0, 0, 1, 0);
			bool hover_saturation = TriangleTests::Intersects(O, D, _A, _B, _C, distTri);

			bool dragged = false;

			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (hover_hue)
				{
					colorpickerstate = CPS_HUE;
					dragged = true;
				}
				else if (hover_saturation)
				{
					colorpickerstate = CPS_SATURATION;
					dragged = true;
				}
			}

			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (colorpickerstate > CPS_IDLE)
				{
					// continue drag if already grabbed whether it is intersecting or not
					dragged = true;
				}
			}
			else
			{
				colorpickerstate = CPS_IDLE;
			}

			if (colorpickerstate == CPS_HUE && dragged)
			{
				//hue pick
				const float angle = wi::math::GetAngle(XMFLOAT2(pointer.x - center.x, pointer.y - center.y), XMFLOAT2(colorpicker_radius, 0));
				hue = angle / XM_2PI * 360.0f;
				Activate();
			}
			else if (colorpickerstate == CPS_SATURATION && dragged)
			{
				// saturation pick
				float u, v, w;
				wi::math::GetBarycentric(O, _A, _B, _C, u, v, w, true);
				// u = saturated corner (color)
				// v = desaturated corner (white)
				// w = no luminosity corner (black)

				hsv source;
				source.h = hue;
				source.s = 1;
				source.v = 1;
				rgb result = hsv2rgb(source);

				XMVECTOR color_corner = XMVectorSet(result.r, result.g, result.b, 1);
				XMVECTOR white_corner = XMVectorSet(1, 1, 1, 1);
				XMVECTOR black_corner = XMVectorSet(0, 0, 0, 1);
				XMVECTOR inner_point = u * color_corner + v * white_corner + w * black_corner;

				result.r = XMVectorGetX(inner_point);
				result.g = XMVectorGetY(inner_point);
				result.b = XMVectorGetZ(inner_point);
				source = rgb2hsv(result);

				saturation = source.s;
				luminance = source.v;

				Activate();
			}
			else if (state != IDLE)
			{
				Deactivate();
			}

			wi::Color color = GetPickColor();
			text_R.SetValue((int)color.getR());
			text_G.SetValue((int)color.getG());
			text_B.SetValue((int)color.getB());
			text_H.SetValue(int(hue));
			text_S.SetValue(int(saturation * 100));
			text_V.SetValue(int(luminance * 100));
			text_hex.SetText(color.to_hex());

			if (dragged)
			{
				FireEvents();
			}
		}
	}
	void ColorPicker::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Window::Render(canvas, cmd);

		if (!IsVisible() || IsMinimized())
		{
			return;
		}

		GraphicsDevice* device = wi::graphics::GetDevice();

		struct Vertex
		{
			XMFLOAT4 pos;
			XMFLOAT4 col;
		};
		static wi::graphics::GPUBuffer vb_hue;
		static wi::graphics::GPUBuffer vb_picker_saturation;
		static wi::graphics::GPUBuffer vb_picker_hue;
		static wi::graphics::GPUBuffer vb_preview;

		static wi::vector<Vertex> vertices_saturation;

		static bool buffersComplete = false;
		if (!buffersComplete)
		{
			buffersComplete = true;

			// saturation
			{
				vertices_saturation.push_back({ XMFLOAT4(0,0,0,0),XMFLOAT4(1,0,0,1) });	// hue
				vertices_saturation.push_back({ XMFLOAT4(0,0,0,0),XMFLOAT4(1,1,1,1) });	// white
				vertices_saturation.push_back({ XMFLOAT4(0,0,0,0),XMFLOAT4(0,0,0,1) });	// black
				wi::math::ConstructTriangleEquilateral(colorpicker_radius_triangle, vertices_saturation[0].pos, vertices_saturation[1].pos, vertices_saturation[2].pos);

				// create alpha blended edge:
				vertices_saturation.push_back(vertices_saturation[0]); // outer
				vertices_saturation.push_back(vertices_saturation[0]); // inner
				vertices_saturation.push_back(vertices_saturation[1]); // outer
				vertices_saturation.push_back(vertices_saturation[1]); // inner
				vertices_saturation.push_back(vertices_saturation[2]); // outer
				vertices_saturation.push_back(vertices_saturation[2]); // inner
				vertices_saturation.push_back(vertices_saturation[0]); // outer
				vertices_saturation.push_back(vertices_saturation[0]); // inner
				wi::math::ConstructTriangleEquilateral(colorpicker_radius_triangle + 4, vertices_saturation[3].pos, vertices_saturation[5].pos, vertices_saturation[7].pos); // extrude outer
				vertices_saturation[9].pos = vertices_saturation[3].pos; // last outer
			}
			// hue
			{
				const float edge = 2.0f;
				wi::vector<Vertex> vertices;
				uint32_t segment_count = 100;
				// inner alpha blended edge
				for (uint32_t i = 0; i <= segment_count; ++i)
				{
					float p = float(i) / segment_count;
					float t = p * XM_2PI;
					float x = cos(t);
					float y = -sin(t);
					hsv source;
					source.h = p * 360.0f;
					source.s = 1;
					source.v = 1;
					rgb result = hsv2rgb(source);
					XMFLOAT4 color = XMFLOAT4(result.r, result.g, result.b, 1);
					XMFLOAT4 coloralpha = XMFLOAT4(result.r, result.g, result.b, 0);
					vertices.push_back({ XMFLOAT4((colorpicker_radius - edge) * x, (colorpicker_radius - edge) * y, 0, 1), coloralpha });
					vertices.push_back({ XMFLOAT4(colorpicker_radius * x, colorpicker_radius * y, 0, 1), color });
				}
				// middle hue
				for (uint32_t i = 0; i <= segment_count; ++i)
				{
					float p = float(i) / segment_count;
					float t = p * XM_2PI;
					float x = cos(t);
					float y = -sin(t);
					hsv source;
					source.h = p * 360.0f;
					source.s = 1;
					source.v = 1;
					rgb result = hsv2rgb(source);
					XMFLOAT4 color = XMFLOAT4(result.r, result.g, result.b, 1);
					vertices.push_back({ XMFLOAT4(colorpicker_radius * x, colorpicker_radius * y, 0, 1), color });
					vertices.push_back({ XMFLOAT4((colorpicker_radius + colorpicker_width) * x, (colorpicker_radius + colorpicker_width) * y, 0, 1), color });
				}
				// outer alpha blended edge
				for (uint32_t i = 0; i <= segment_count; ++i)
				{
					float p = float(i) / segment_count;
					float t = p * XM_2PI;
					float x = cos(t);
					float y = -sin(t);
					hsv source;
					source.h = p * 360.0f;
					source.s = 1;
					source.v = 1;
					rgb result = hsv2rgb(source);
					XMFLOAT4 color = XMFLOAT4(result.r, result.g, result.b, 1);
					XMFLOAT4 coloralpha = XMFLOAT4(result.r, result.g, result.b, 0);
					vertices.push_back({ XMFLOAT4((colorpicker_radius + colorpicker_width) * x, (colorpicker_radius + colorpicker_width) * y, 0, 1), color });
					vertices.push_back({ XMFLOAT4((colorpicker_radius + colorpicker_width + edge) * x, (colorpicker_radius + colorpicker_width + edge) * y, 0, 1), coloralpha });
				}

				GPUBufferDesc desc;
				desc.bind_flags = BindFlag::VERTEX_BUFFER;
				desc.size = vertices.size() * sizeof(Vertex);
				desc.stride = 0;
				device->CreateBuffer(&desc, vertices.data(), &vb_hue);
			}
			// saturation picker (small circle)
			{
				float _radius = 3;
				float _width = 3;
				wi::vector<Vertex> vertices;
				uint32_t segment_count = 100;
				for (uint32_t i = 0; i <= segment_count; ++i)
				{
					float p = float(i) / 100;
					float t = p * XM_2PI;
					float x = cos(t);
					float y = -sin(t);
					vertices.push_back({ XMFLOAT4(_radius * x, _radius * y, 0, 1), XMFLOAT4(1,1,1,1) });
					vertices.push_back({ XMFLOAT4((_radius + _width) * x, (_radius + _width) * y, 0, 1), XMFLOAT4(1,1,1,1) });
				}

				GPUBufferDesc desc;
				desc.bind_flags = BindFlag::VERTEX_BUFFER;
				desc.size = vertices.size() * sizeof(Vertex);
				desc.stride = 0;
				device->CreateBuffer(&desc, vertices.data(), &vb_picker_saturation);
			}
			// hue picker (rectangle)
			{
				float boldness = 4.0f;
				float halfheight = 8.0f;
				Vertex vertices[] = {
					// left side:
					{ XMFLOAT4(colorpicker_radius - boldness, -halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius, -halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius - boldness, halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius, halfheight, 0, 1),XMFLOAT4(1,1,1,1) },

					// bottom side:
					{ XMFLOAT4(colorpicker_radius - boldness, halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius - boldness, halfheight - boldness, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius + colorpicker_width + boldness, halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius + colorpicker_width + boldness, halfheight - boldness, 0, 1),XMFLOAT4(1,1,1,1) },

					// right side:
					{ XMFLOAT4(colorpicker_radius + colorpicker_width + boldness, halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius + colorpicker_width, halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius + colorpicker_width + boldness, -halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius + colorpicker_width, -halfheight, 0, 1),XMFLOAT4(1,1,1,1) },

					// top side:
					{ XMFLOAT4(colorpicker_radius + colorpicker_width + boldness, -halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius + colorpicker_width + boldness, -halfheight + boldness, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius - boldness, -halfheight, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(colorpicker_radius - boldness, -halfheight + boldness, 0, 1),XMFLOAT4(1,1,1,1) },
				};

				GPUBufferDesc desc;
				desc.bind_flags = BindFlag::VERTEX_BUFFER;
				desc.size = sizeof(vertices);
				desc.stride = 0;
				device->CreateBuffer(&desc, vertices, &vb_picker_hue);
			}
			// preview
			{
				float _width = 50;
				Vertex vertices[] = {
					{ XMFLOAT4(-_width, _width, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(0, _width, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(-_width, 0, 0, 1),XMFLOAT4(1,1,1,1) },
					{ XMFLOAT4(0, 0, 0, 1),XMFLOAT4(1,1,1,1) },
				};

				GPUBufferDesc desc;
				desc.bind_flags = BindFlag::VERTEX_BUFFER;
				desc.size = sizeof(vertices);
				device->CreateBuffer(&desc, vertices, &vb_preview);
			}

		}

		const wi::Color final_color = GetPickColor();
		const float angle = hue / 360.0f * XM_2PI;

		const XMMATRIX Projection = canvas.GetProjection();

		device->BindPipelineState(&gui_internal().PSO_colored, cmd);

		ApplyScissor(canvas, scissorRect, cmd);

		float sca = std::min(scale.x / cp_width, scale.y / cp_height);

		XMMATRIX W =
			XMMatrixScaling(sca, sca, 1) *
			XMMatrixTranslation(translation.x + scale.x * 0.4f, translation.y + scale.y * 0.5f, 0)
			;

		MiscCB cb;

		// render saturation triangle
		{
			hsv source;
			source.h = hue;
			source.s = 1;
			source.v = 1;
			rgb result = hsv2rgb(source);
			vertices_saturation[0].col = XMFLOAT4(result.r, result.g, result.b, 1);

			vertices_saturation[3].col = vertices_saturation[0].col; vertices_saturation[3].col.w = 0;
			vertices_saturation[4].col = vertices_saturation[0].col;
			vertices_saturation[5].col = vertices_saturation[1].col; vertices_saturation[5].col.w = 0;
			vertices_saturation[6].col = vertices_saturation[1].col;
			vertices_saturation[7].col = vertices_saturation[2].col; vertices_saturation[7].col.w = 0;
			vertices_saturation[8].col = vertices_saturation[2].col;
			vertices_saturation[9].col = vertices_saturation[0].col; vertices_saturation[9].col.w = 0;
			vertices_saturation[10].col = vertices_saturation[0].col;

			size_t alloc_size = sizeof(Vertex) * vertices_saturation.size();
			GraphicsDevice::GPUAllocation vb_saturation = device->AllocateGPU(alloc_size, cmd);
			memcpy(vb_saturation.data, vertices_saturation.data(), alloc_size);

			XMStoreFloat4x4(&cb.g_xTransform,
				XMMatrixRotationZ(-angle) *
				W *
				Projection
			);
			cb.g_xColor = IsEnabled() ? float4(1, 1, 1, 1) : float4(0.5f, 0.5f, 0.5f, 1);
			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
			const GPUBuffer* vbs[] = {
				&vb_saturation.buffer,
			};
			const uint32_t strides[] = {
				sizeof(Vertex),
			};
			const uint64_t offsets[] = {
				vb_saturation.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);
			device->Draw((uint32_t)vertices_saturation.size(), 0, cmd);
		}

		// render hue circle
		{
			XMStoreFloat4x4(&cb.g_xTransform,
				W *
				Projection
			);
			cb.g_xColor = IsEnabled() ? float4(1, 1, 1, 1) : float4(0.5f, 0.5f, 0.5f, 1);
			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
			const GPUBuffer* vbs[] = {
				&vb_hue,
			};
			const uint32_t strides[] = {
				sizeof(Vertex),
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
			device->Draw((uint32_t)(vb_hue.GetDesc().size / sizeof(Vertex)), 0, cmd);
		}

		// render hue picker
		if (IsEnabled())
		{
			XMStoreFloat4x4(&cb.g_xTransform,
				XMMatrixRotationZ(-hue / 360.0f * XM_2PI) *
				W *
				Projection
			);

			hsv source;
			source.h = hue;
			source.s = 1;
			source.v = 1;
			rgb result = hsv2rgb(source);
			cb.g_xColor = float4(1 - result.r, 1 - result.g, 1 - result.b, 1);

			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
			const GPUBuffer* vbs[] = {
				&vb_picker_hue,
			};
			const uint32_t strides[] = {
				sizeof(Vertex),
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
			device->Draw((uint32_t)(vb_picker_hue.GetDesc().size / sizeof(Vertex)), 0, cmd);
		}

		// render saturation picker
		if (IsEnabled())
		{
			XMFLOAT4 A, B, C;
			wi::math::ConstructTriangleEquilateral(colorpicker_radius_triangle, A, B, C);
			XMVECTOR _A = XMLoadFloat4(&A);
			XMVECTOR _B = XMLoadFloat4(&B);
			XMVECTOR _C = XMLoadFloat4(&C);
			XMMATRIX _triTransform = XMMatrixRotationZ(-hue / 360.0f * XM_2PI);
			_A = XMVector4Transform(_A, _triTransform);
			_B = XMVector4Transform(_B, _triTransform);
			_C = XMVector4Transform(_C, _triTransform);

			hsv source;
			source.h = hue;
			source.s = 1;
			source.v = 1;
			rgb result = hsv2rgb(source);

			XMVECTOR color_corner = XMVectorSet(result.r, result.g, result.b, 1);
			XMVECTOR white_corner = XMVectorSet(1, 1, 1, 1);
			XMVECTOR black_corner = XMVectorSet(0, 0, 0, 1);

			source.h = hue;
			source.s = saturation;
			source.v = luminance;
			result = hsv2rgb(source);
			XMVECTOR inner_point = XMVectorSet(result.r, result.g, result.b, 1);

			float u, v, w;
			wi::math::GetBarycentric(inner_point, color_corner, white_corner, black_corner, u, v, w, true);

			XMVECTOR picker_center = u * _A + v * _B + w * _C;

			XMStoreFloat4x4(&cb.g_xTransform,
				XMMatrixTranslationFromVector(picker_center) *
				W *
				Projection
			);
			cb.g_xColor = float4(1 - final_color.toFloat3().x, 1 - final_color.toFloat3().y, 1 - final_color.toFloat3().z, 1);
			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
			const GPUBuffer* vbs[] = {
				&vb_picker_saturation,
			};
			const uint32_t strides[] = {
				sizeof(Vertex),
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
			device->Draw((uint32_t)(vb_picker_saturation.GetDesc().size / sizeof(Vertex)), 0, cmd);
		}

		// render preview
		{
			XMStoreFloat4x4(&cb.g_xTransform,
				XMMatrixTranslation(translation.x + scale.x - sca - 2, translation.y + control_size + 4 + 22, 0) *
				Projection
			);
			cb.g_xColor = final_color.toFloat4();
			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
			const GPUBuffer* vbs[] = {
				&vb_preview,
			};
			const uint32_t strides[] = {
				sizeof(Vertex),
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
			device->Draw((uint32_t)(vb_preview.GetDesc().size / sizeof(Vertex)), 0, cmd);
		}
	}
	void ColorPicker::ResizeLayout()
	{
		wi::gui::Window::ResizeLayout();
		const float padding = 4;
		const float width = GetWidgetAreaSize().x;
		float y = GetWidgetAreaSize().y - control_size;
		float jump = 20;

		auto add = [&](wi::gui::Widget& widget) {
			if (!widget.IsVisible())
				return;
			const float margin_left = 20;
			const float margin_right = 80;
			y -= widget.GetSize().y;
			y -= padding;
			widget.SetPos(XMFLOAT2(margin_left, y));
			widget.SetSize(XMFLOAT2(width - margin_left - margin_right, widget.GetSize().y));
			};
		auto add_right = [&](wi::gui::Widget& widget) {
			if (!widget.IsVisible())
				return;
			const float margin_right = padding;
			y -= widget.GetSize().y;
			y -= padding;
			widget.SetPos(XMFLOAT2(width - margin_right - widget.GetSize().x, y));
			};

		add(alphaSlider);
		y += alphaSlider.GetSize().y;
		y += padding;

		add_right(text_V);
		add_right(text_S);
		add_right(text_H);

		y -= jump;

		add_right(text_B);
		add_right(text_G);
		add_right(text_R);

		y = control_size + 4;
		add_right(text_hex);
	}
	void ColorPicker::SetColor(wi::Color color, int id)
	{
		Window::SetColor(color, id);

		if (id == WIDGET_ID_COLORPICKER_BASE)
		{
			sprites[IDLE].params.color = color;
		}
	}
	void ColorPicker::SetImage(wi::Resource resource, int id)
	{
		Window::SetImage(resource, id);

		if (id == WIDGET_ID_COLORPICKER_BASE)
		{
			sprites[IDLE].textureResource = resource;
		}
	}
	wi::Color ColorPicker::GetPickColor() const
	{
		hsv source;
		source.h = hue;
		source.s = saturation;
		source.v = luminance;
		rgb result = hsv2rgb(source);
		return wi::Color::fromFloat4(XMFLOAT4(result.r, result.g, result.b, alphaSlider.GetValue() / 255.0f));
	}
	void ColorPicker::SetPickColor(wi::Color value)
	{
		if (colorpickerstate != CPS_IDLE)
		{
			// Only allow setting the pick color when picking is not active, the RGB and HSV precision combat each other
			return;
		}

		rgb source;
		source.r = value.toFloat3().x;
		source.g = value.toFloat3().y;
		source.b = value.toFloat3().z;
		hsv result = rgb2hsv(source);
		if (value.getR() != value.getG() || value.getG() != value.getB() || value.getR() != value.getB())
		{
			hue = result.h; // only change the hue when not pure greyscale because those don't retain the saturation value
		}
		saturation = result.s;
		luminance = result.v;
		alphaSlider.SetValue((float)value.getA());
	}
	void ColorPicker::FireEvents()
	{
		if (onColorChanged == nullptr)
			return;
		EventArgs args = {};
		args.color = GetPickColor();
		onColorChanged(args);
	}
	void ColorPicker::OnColorChanged(std::function<void(EventArgs args)> func)
	{
		onColorChanged = func;
	}




	constexpr float item_height() { return 20.0f; }
	void TreeList::Create(const std::string& name)
	{
		SetName(name);
		SetText(name);
		OnSelect([](EventArgs args) {});

		SetColor(wi::Color(100, 100, 100, 100), wi::gui::IDLE);
		for (int i = FOCUS + 1; i < WIDGETSTATE_COUNT; ++i)
		{
			sprites[i].params.color = sprites[FOCUS].params.color;
			scrollbar.sprites[i].params.color = sprites[FOCUS].params.color;
		}
		font.params.v_align = wi::font::WIFALIGN_CENTER;

		scrollbar.SetColor(wi::Color(80, 80, 80, 100), wi::gui::IDLE);
		scrollbar.sprites_knob[ScrollBar::SCROLLBAR_INACTIVE].params.color = wi::Color(140, 140, 140, 140);
		scrollbar.sprites_knob[ScrollBar::SCROLLBAR_HOVER].params.color = wi::Color(180, 180, 180, 180);
		scrollbar.sprites_knob[ScrollBar::SCROLLBAR_GRABBED].params.color = wi::Color::White();
		scrollbar.SetOverScroll(0.25f);

		SetSize(XMFLOAT2(100, 200));
	}
	bool TreeList::DoesItemHaveChildren(int index) const
	{
		if (items.size() <= size_t(index + 1)) // if item doesn't exist or last then no children
			return false;
		if (items[index].level + 1 == items[index + 1].level) // if item after index is exactly one level down, then it is its child
			return true;
		return false;
	}
	float TreeList::GetItemOffset(int index) const
	{
		return 2 + scrollbar.GetOffset() + index * item_height();
	}
	Hitbox2D TreeList::GetHitbox_ListArea() const
	{
		Hitbox2D retval = Hitbox2D(XMFLOAT2(translation.x, translation.y + item_height() + 1), XMFLOAT2(scale.x, scale.y - item_height() - 1));
		if (scrollbar.IsScrollbarRequired())
		{
			retval.siz.x -= scrollbar.scale.x + 1;
		}
		return retval;
	}
	Hitbox2D TreeList::GetHitbox_Item(int visible_count, int level) const
	{
		XMFLOAT2 pos = XMFLOAT2(translation.x + 2 + level * item_height(), translation.y + GetItemOffset(visible_count) + item_height() * 0.5f);
		Hitbox2D hitbox;
		hitbox.pos = XMFLOAT2(pos.x + item_height() * 0.5f + 2, pos.y - item_height() * 0.5f);
		hitbox.siz = XMFLOAT2(scale.x - 2 - item_height() * 0.5f - 2 - level * item_height() - 2, item_height());
		if (HasScrollbar())
		{
			hitbox.siz.x -= scrollbar.scale.x;
		}
		return hitbox;
	}
	Hitbox2D TreeList::GetHitbox_ItemOpener(int visible_count, int level) const
	{
		XMFLOAT2 pos = XMFLOAT2(translation.x + 2 + level * item_height(), translation.y + GetItemOffset(visible_count) + item_height() * 0.5f);
		Hitbox2D hb = Hitbox2D(XMFLOAT2(pos.x, pos.y - item_height() * 0.25f), XMFLOAT2(item_height() * 0.5f, item_height() * 0.5f));
		HitboxConstrain(hb);
		return hb;
	}
	bool TreeList::HasScrollbar() const
	{
		return scale.y < (int)items.size() * item_height();
	}
	void TreeList::Update(const wi::Canvas& canvas, float dt)
	{
		if (!IsVisible())
		{
			return;
		}

		Widget::Update(canvas, dt);

		// Resizer updates:
		if (IsEnabled())
		{
			Hitbox2D pointerHitbox = GetPointerHitbox(false);

			float vscale = scale.y;
			Hitbox2D bottomhitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y + vscale), XMFLOAT2(scale.x, resizehitboxwidth));

			if (resize_state == RESIZE_STATE_NONE && wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (pointerHitbox.intersects(bottomhitbox))
				{
					resize_state = RESIZE_STATE_BOTTOM;
					Activate();
					force_disable = true;
				}
				resize_begin = pointerHitbox.pos;
			}
			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				if (resize_state != RESIZE_STATE_NONE)
				{
					auto saved_parent = this->parent;
					this->Detach();
					float deltaX = pointerHitbox.pos.x - resize_begin.x;
					float deltaY = pointerHitbox.pos.y - resize_begin.y;

					switch (resize_state)
					{
					case RESIZE_STATE_BOTTOM:
						this->Scale(XMFLOAT3(1, (scale.y + deltaY) / scale.y, 1));
						break;
					default:
						break;
					}

					this->scale_local = wi::math::Max(this->scale_local, XMFLOAT3(item_height() * 3, item_height() * 3, 1)); // don't allow resize to negative or too small
					this->scale = this->scale_local;
					this->AttachTo(saved_parent);
					resize_begin = pointerHitbox.pos;
				}
			}
			else
			{
				resize_state = RESIZE_STATE_NONE;
			}

			if (
				resize_state != RESIZE_STATE_NONE ||
				pointerHitbox.intersects(bottomhitbox)
				)
			{
				resize_blink_timer += dt;
			}
			else
			{
				resize_blink_timer = 0;
			}
		}

		active_area.pos.x = float(scissorRect.left);
		active_area.pos.y = float(scissorRect.top);
		active_area.siz.x = float(scissorRect.right) - float(scissorRect.left);
		active_area.siz.y = float(scissorRect.bottom) - float(scissorRect.top);

		Hitbox2D pointerHitbox = GetPointerHitbox();

		ComputeScrollbarLength();

		const float scrollbar_width = 12;
		scrollbar.SetSize(XMFLOAT2(scrollbar_width - 1, scale.y - 1 - item_height()));
		scrollbar.SetPos(XMFLOAT2(translation.x + 1 + scale.x - scrollbar_width, translation.y + 1 + item_height()));
		scrollbar.Update(canvas, dt);
		if (scrollbar.GetState() > IDLE)
		{
			Deactivate();
		}

		if (IsEnabled() && dt > 0)
		{
			if (state == FOCUS)
			{
				state = IDLE;
			}
			if (state == DEACTIVATING)
			{
				state = IDLE;
			}
			if (state == ACTIVE)
			{
				Deactivate();
			}

			Hitbox2D hitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y), XMFLOAT2(scale.x, scale.y));

			if (state == IDLE && hitbox.intersects(pointerHitbox))
			{
				state = FOCUS;
			}

			bool clicked = false;
			if (wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
			{
				clicked = true;
			}

			if (onDelete && state == FOCUS && wi::input::Press(wi::input::KEYBOARD_BUTTON_DELETE) && !typing_active)
			{
				int index = 0;
				for (auto& item : items)
				{
					if (item.selected)
					{
						EventArgs args;
						args.iValue = index;
						args.sValue = items[index].name;
						args.userdata = items[index].userdata;
						onDelete(args);
					}
					index++;
				}
			}

			bool click_down = false;
			if (wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				click_down = true;
				if (state == FOCUS || state == DEACTIVATING)
				{
					// Keep pressed until mouse is released
					Activate();
				}
			}

			float scroll = wi::input::GetPointer().z * 10;
			if (scroll && scroll_allowed && scrollbar.IsScrollbarRequired() && pointerHitbox.intersects(hitBox))
			{
				scroll_allowed = false;
				// This is outside scrollbar code, because it can also be scrolled if parent widget is only in focus
				scrollbar.Scroll(scroll);
			}

			Hitbox2D itemlist_box = GetHitbox_ListArea();

			tooltipFont.text.clear();

			// control-list
			item_highlight = -1;
			opener_highlight = -1;
			int first_selected = -1;
			if (scrollbar.GetState() == IDLE)
			{
				int i = -1;
				int visible_count = 0;
				int parent_level = 0;
				bool parent_open = true;
				for (Item& item : items)
				{
					i++;
					if (item.selected && first_selected < 0)
					{
						first_selected = i;
					}
					if (!parent_open && item.level > parent_level)
					{
						continue;
					}
					visible_count++;
					parent_open = item.open;
					parent_level = item.level;

					Hitbox2D open_box = GetHitbox_ItemOpener(visible_count, item.level);
					if (!open_box.intersects(itemlist_box))
					{
						continue;
					}

					if (open_box.intersects(pointerHitbox))
					{
						// Opened flag box:
						opener_highlight = i;
						if (clicked)
						{
							item.open = !item.open;
							Activate();
						}
					}
					else
					{
						// Item name box:
						Hitbox2D name_box = GetHitbox_Item(visible_count, item.level);
						if (name_box.intersects(pointerHitbox))
						{
							item_highlight = i;
							SetTooltip(item.name);
							if (clicked)
							{
								if (wi::input::IsDoubleClicked() && onDoubleClick != nullptr)
								{
									EventArgs args;
									args.iValue = i;
									args.sValue = items[i].name;
									args.userdata = items[i].userdata;
									onDoubleClick(args);
								}
								else
								{
									if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LCONTROL) || wi::input::Down(wi::input::KEYBOARD_BUTTON_RCONTROL))
									{
										// control: nothing
									}
									else if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LSHIFT) || wi::input::Down(wi::input::KEYBOARD_BUTTON_RSHIFT))
									{
										if (first_selected >= 0)
										{
											// shift: select range from first selected up to current:
											for (int j = first_selected; j < i; ++j)
											{
												Select(j, false);
											}
										}
									}
									else
									{
										// no control, no shift then only this will be selected
										ClearSelection();
									}
									Select(i);
								}
								Activate();
							}
						}
					}
				}
			}
		}

		if (state == IDLE && resize_blink_timer > 0)
			state = FOCUS;

		sprites[state].params.siz.y = item_height();
		font.params.posX = translation.x + 2;
		font.params.posY = translation.y + sprites[state].params.siz.y * 0.5f;
	}
	void TreeList::Render(const wi::Canvas& canvas, CommandList cmd) const
	{
		Widget::Render(canvas, cmd);
		if (!IsVisible())
		{
			return;
		}
		GraphicsDevice* device = wi::graphics::GetDevice();

		// shadow:
		if (shadow > 0)
		{
			wi::image::Params fx = sprites[state].params;
			fx.gradient = wi::image::Params::Gradient::None;
			fx.pos.x -= shadow;
			fx.pos.y -= shadow;
			fx.siz.x += shadow * 2;
			fx.siz.y = scale.y + shadow * 2;
			fx.color = shadow_color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += shadow;
					}
				}
			}
			if (shadow_highlight)
			{
				fx.enableHighlight();
				fx.highlight_pos = GetPointerHighlightPos(canvas);
				fx.highlight_color = shadow_highlight_color;
				fx.highlight_spread = shadow_highlight_spread;
			}
			else
			{
				fx.disableHighlight();
			}
			wi::image::Draw(nullptr, fx, cmd);
		}

		// resize indicator:
		{
			// hitboxes are recomputed because window transform might have changed since update!!
			float vscale = scale.y;
			Hitbox2D bottomhitbox = Hitbox2D(XMFLOAT2(translation.x, translation.y + vscale), XMFLOAT2(scale.x, resizehitboxwidth));

			const Hitbox2D pointerHitbox = GetPointerHitbox(false);

			wi::image::Params fx = sprites[state].params;
			fx.blendFlag = wi::enums::BLENDMODE_ALPHA;
			fx.pos.x -= resizehitboxwidth;
			fx.pos.y -= resizehitboxwidth;
			fx.siz.x += resizehitboxwidth * 2;
			fx.siz.y = scale.y + resizehitboxwidth * 2;
			fx.color = resize_state == RESIZE_STATE_NONE ? sprites[FOCUS].params.color : sprites[ACTIVE].params.color;
			if (fx.isCornerRoundingEnabled())
			{
				for (auto& corner_rounding : fx.corners_rounding)
				{
					if (corner_rounding.radius > 0)
					{
						corner_rounding.radius += resizehitboxwidth;
					}
				}
			}
			//fx.border_soften = 0.01f;

			if (resize_state == RESIZE_STATE_BOTTOM || pointerHitbox.intersects(bottomhitbox))
			{
				fx.angular_softness_outer_angle = XM_PI * 0.25f;
				fx.angular_softness_inner_angle = XM_PI * wi::math::Lerp(0.0f, 0.24f, std::abs(std::sin(resize_blink_timer * 4)));
				fx.angular_softness_direction = XMFLOAT2(0, 1);
				wi::image::Draw(nullptr, fx, cmd);
				wi::input::SetCursor(wi::input::CURSOR_RESIZE_NS);
			}
		}

		// control-base
		sprites[state].Draw(cmd);

		ApplyScissor(canvas, scissorRect, cmd);

		font.Draw(cmd);

		scrollbar.Render(canvas, cmd);

		// list background
		Hitbox2D itemlist_box = GetHitbox_ListArea();
		wi::image::Params fx = sprites[state].params;
		fx.color = sprites[IDLE].params.color;
		fx.pos = XMFLOAT3(itemlist_box.pos.x, itemlist_box.pos.y, 0);
		fx.siz = XMFLOAT2(itemlist_box.siz.x, itemlist_box.siz.y);
		wi::image::Draw(nullptr, fx, cmd);

		Rect rect_without_scrollbar;
		rect_without_scrollbar.left = (int)itemlist_box.pos.x;
		rect_without_scrollbar.right = (int)(itemlist_box.pos.x + itemlist_box.siz.x);
		rect_without_scrollbar.top = (int)itemlist_box.pos.y;
		rect_without_scrollbar.bottom = (int)(itemlist_box.pos.y + itemlist_box.siz.y);
		ApplyScissor(canvas, rect_without_scrollbar, cmd);

		struct Vertex
		{
			XMFLOAT4 pos;
			XMFLOAT4 col;
		};
		static GPUBuffer vb_triangle;
		if (!vb_triangle.IsValid())
		{
			Vertex vertices[3];
			vertices[0].col = XMFLOAT4(1, 1, 1, 1);
			vertices[1].col = XMFLOAT4(1, 1, 1, 1);
			vertices[2].col = XMFLOAT4(1, 1, 1, 1);
			wi::math::ConstructTriangleEquilateral(1, vertices[0].pos, vertices[1].pos, vertices[2].pos);

			GPUBufferDesc desc;
			desc.bind_flags = BindFlag::VERTEX_BUFFER;
			desc.size = sizeof(vertices);
			device->CreateBuffer(&desc, vertices, &vb_triangle);
		}
		const XMMATRIX Projection = canvas.GetProjection();

		// control-list
		int i = -1;
		int visible_count = 0;
		int parent_level = 0;
		bool parent_open = true;
		for (const Item& item : items)
		{
			i++;
			if (!parent_open && item.level > parent_level)
			{
				continue;
			}
			visible_count++;
			parent_open = item.open;
			parent_level = item.level;

			Hitbox2D open_box = GetHitbox_ItemOpener(visible_count, item.level);
			if (!open_box.intersects(itemlist_box))
			{
				continue;
			}

			Hitbox2D name_box = GetHitbox_Item(visible_count, item.level);

			// selected box:
			if (item.selected || item_highlight == i)
			{
				wi::image::Draw(nullptr
					, wi::image::Params(name_box.pos.x, name_box.pos.y, name_box.siz.x, name_box.siz.y,
						sprites[item.selected ? FOCUS : IDLE].params.color), cmd);
			}

			// opened flag triangle:
			if(DoesItemHaveChildren(i))
			{
				device->BindPipelineState(&gui_internal().PSO_colored, cmd);

				MiscCB cb;
				cb.g_xColor = opener_highlight == i ? wi::Color::White().toFloat4() : sprites[FOCUS].params.color;
				XMStoreFloat4x4(&cb.g_xTransform, XMMatrixScaling(item_height() * 0.3f, item_height() * 0.3f, 1) *
					XMMatrixRotationZ(item.open ? XM_PIDIV2 : 0) *
					XMMatrixTranslation(open_box.pos.x + open_box.siz.x * 0.5f, open_box.pos.y + open_box.siz.y * 0.25f, 0) *
					Projection
				);
				device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_MISC, cmd);
				const GPUBuffer* vbs[] = {
					&vb_triangle,
				};
				const uint32_t strides[] = {
					sizeof(Vertex),
				};
				device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);

				device->Draw(3, 0, cmd);
			}

			// Item name text:
			wi::font::Params fp = wi::font::Params(
				name_box.pos.x + 1,
				name_box.pos.y + name_box.siz.y * 0.5f,
				wi::font::WIFONTSIZE_DEFAULT,
				wi::font::WIFALIGN_LEFT,
				wi::font::WIFALIGN_CENTER,
				font.params.color,
				font.params.shadowColor
			);
			fp.style = font.params.style;
			wi::font::Draw(item.name, fp, cmd);
		}
	}
	void TreeList::OnSelect(std::function<void(EventArgs args)> func)
	{
		onSelect = func;
	}
	void TreeList::OnDelete(std::function<void(EventArgs args)> func)
	{
		onDelete = func;
	}
	void TreeList::OnDoubleClick(std::function<void(EventArgs args)> func)
	{
		onDoubleClick = func;
	}
	void TreeList::AddItem(const Item& item)
	{
		items.push_back(item);
	}
	void TreeList::AddItem(const std::string& name)
	{
		Item item;
		item.name = name;
		AddItem(item);
	}
	void TreeList::ClearItems()
	{
		items.clear();
	}
	void TreeList::ClearSelection()
	{
		for (Item& item : items)
		{
			item.selected = false;
		}

		EventArgs args = {};
		args.iValue = -1;
		onSelect(args);
	}
	void TreeList::Select(int index, bool allow_deselect)
	{
		int selected_count = 0;
		if (allow_deselect)
		{
			for (auto& x : items)
			{
				if (x.selected)
					selected_count++;
			}
		}

		if (selected_count > 1)
		{
			// If multiple are selected, then we can deselect:
			items[index].selected = !items[index].selected;
		}
		else
		{
			items[index].selected = true;
		}

		EventArgs args;
		args.iValue = index;
		args.sValue = items[index].name;
		args.userdata = items[index].userdata;
		onSelect(args);
	}
	void TreeList::ComputeScrollbarLength()
	{
		float scroll_length = 0;
		{
			int parent_level = 0;
			bool parent_open = true;
			for (const Item& item : items)
			{
				if (!parent_open && item.level > parent_level)
				{
					continue;
				}
				parent_open = item.open;
				parent_level = item.level;
				scroll_length += item_height();
			}
		}
		scrollbar.SetListLength(scroll_length);
		scrollbar.Update({}, 0);
	}
	void TreeList::FocusOnItem(int index)
	{
		if (index < 0 || index >= items.size())
			return;

		// Open parent items of target:
		int target = index;
		int target_level = items[target].level;
		while (target_level > 0)
		{
			if (items[target - 1].level == target_level - 1)
			{
				items[target - 1].open = true;
				target_level--;
			}
			target--;
		}

		// Recompute scrollbar after opened tree leading to target item:
		ComputeScrollbarLength();

		// Count visible items before target:
		int visible_count = 0;
		int parent_level = 0;
		bool parent_open = true;
		for (int i = 0; i <= index; ++i)
		{
			auto& item = items[i];
			if (!parent_open && item.level > parent_level)
			{
				continue;
			}
			visible_count++;
			parent_open = item.open;
			parent_level = item.level;
		}

		// Set scrollbar offset:
		float offset = visible_count * item_height();
		scrollbar.SetOffset(offset);
	}
	void TreeList::FocusOnItemByUserdata(uint64_t userdata)
	{
		for (size_t i = 0; i < items.size(); ++i)
		{
			if (items[i].userdata == userdata)
			{
				FocusOnItem(int(i));
				break;
			}
		}
	}
	const TreeList::Item& TreeList::GetItem(int index) const
	{
		return items[index];
	}
	void TreeList::SetColor(wi::Color color, int id)
	{
		Widget::SetColor(color, id);
		scrollbar.SetColor(color, id);
	}
	void TreeList::SetTheme(const Theme& theme, int id)
	{
		Widget::SetTheme(theme, id);
		scrollbar.SetTheme(theme, id);
	}


}
