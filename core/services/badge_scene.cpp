#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

	const D2D1_COLOR_F badge_red = D2D1::ColorF(220 / 255.f, 30 / 255.f, 40 / 255.f);
	const D2D1_COLOR_F digit_white = D2D1::ColorF(D2D1::ColorF::White);

	bool same_rects(const std::vector<RECT>& a, const std::vector<RECT>& b)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end(),
			[](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; });
	}

	long long area(const RECT& r)
	{
		return static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
	}

}

int badge_scene::sprite_size(float diameter)
{
	// A pixel of room on each side for the antialiased edge.
	return std::max(1, static_cast<int>(std::lround(diameter))) + 2;
}

badge_scene::cut badge_scene::cut_out(const badge& b, const std::vector<RECT>& visible)
{
	const int size = sprite_size(b.diameter);
	const LONG left = std::lround(b.x - size / 2.0f);
	const LONG top = std::lround(b.y - size / 2.0f);
	cut c{ { left, top, left + size, top + size } };

	long long shown = 0;
	for (const RECT& v : visible) {
		RECT piece{};
		if (!IntersectRect(&piece, &c.sprite, &v)) continue;
		OffsetRect(&piece, -left, -top);
		shown += area(piece);
		c.pieces.push_back(piece);
	}
	// Visible rectangles never overlap, so covering the whole area means nothing hides it.
	if (shown == area(c.sprite)) c.pieces = { RECT{ 0, 0, size, size } };
	return c;
}

badge_scene::badge_scene(dx::device& dev, IDCompositionVisual& root) : dev(dev), root(root)
{
}

bool badge_scene::update(const std::vector<badge_group>& wanted, POINT root_origin)
{
	const auto now = clock::now();
	bool changed = false;

	for (const badge_group& w : wanted) {
		const auto [itr, added] = groups.try_emplace(w.key);
		group& g = itr->second;
		if (added && FAILED(dev.composition().CreateVisual(g.visual.GetAddressOf()))) {
			groups.erase(itr);
			continue;
		}
		g.seen = now;

		const POINT offset{ w.origin.x - root_origin.x, w.origin.y - root_origin.y };
		if (offset.x != g.offset.x || offset.y != g.offset.y) {
			g.visual->SetOffsetX(static_cast<float>(offset.x));
			g.visual->SetOffsetY(static_cast<float>(offset.y));
			g.offset = offset;
			changed = true;
		}
		if (added || w.badges != g.badges || !same_rects(w.visible, g.visible)) changed |= lay_out(g, w);
		if (!g.attached) {
			root.AddVisual(g.visual.Get(), FALSE, nullptr);
			g.attached = true;
			changed = true;
		}
	}

	// Groups that went away come down but stay around: a window brought forward again needs nothing new.
	for (auto itr = groups.begin(); itr != groups.end();) {
		group& g = itr->second;
		if (g.seen != now && g.attached) {
			root.RemoveVisual(g.visual.Get());
			g.attached = false;
			changed = true;
		}
		itr = now - g.seen > keep_hidden ? groups.erase(itr) : std::next(itr);
	}
	return changed;
}

bool badge_scene::lay_out(group& g, const badge_group& wanted)
{
	bool touched = false;
	if (g.slots.size() < wanted.badges.size()) g.slots.resize(wanted.badges.size());
	for (std::size_t i = 0; i < wanted.badges.size(); ++i) touched |= place(g, g.slots[i], wanted.badges[i], wanted.visible);
	for (std::size_t i = wanted.badges.size(); i < g.slots.size(); ++i) touched |= clear(g, g.slots[i]);
	g.slots.resize(wanted.badges.size());
	g.badges = wanted.badges;
	g.visible = wanted.visible;
	return touched;
}

bool badge_scene::place(group& g, std::vector<placed>& slot, const badge& b, const std::vector<RECT>& visible)
{
	const cut c = cut_out(b, visible);
	IDCompositionSurface* picture = c.pieces.empty() ? nullptr : sprite(b);
	if (!picture) return clear(g, slot);

	bool touched = false;
	while (slot.size() > c.pieces.size()) {
		g.visual->RemoveVisual(slot.back().visual.Get());
		slot.pop_back();
		touched = true;
	}
	while (slot.size() < c.pieces.size()) {
		placed p;
		if (FAILED(dev.composition().CreateVisual(p.visual.GetAddressOf()))) break;
		g.visual->AddVisual(p.visual.Get(), FALSE, nullptr);
		slot.push_back(std::move(p));
		touched = true;
	}

	const RECT whole{ 0, 0, c.sprite.right - c.sprite.left, c.sprite.bottom - c.sprite.top };
	for (std::size_t k = 0; k < slot.size(); ++k) {
		placed& p = slot[k];
		if (p.shows != picture) {
			p.visual->SetContent(picture);
			p.shows = picture;
			touched = true;
		}
		if (p.at.x != c.sprite.left || p.at.y != c.sprite.top) {
			p.visual->SetOffsetX(static_cast<float>(c.sprite.left));
			p.visual->SetOffsetY(static_cast<float>(c.sprite.top));
			p.at = { c.sprite.left, c.sprite.top };
			touched = true;
		}

		const RECT& piece = c.pieces[k];
		const bool clip = !EqualRect(&piece, &whole);
		if (clip && (!p.clipped || !EqualRect(&piece, &p.clip))) {
			p.visual->SetClip(D2D1::RectF(static_cast<float>(piece.left), static_cast<float>(piece.top),
				static_cast<float>(piece.right), static_cast<float>(piece.bottom)));
			p.clip = piece;
			p.clipped = true;
			touched = true;
		}
		else if (!clip && p.clipped) {
			p.visual->SetClip(static_cast<IDCompositionClip*>(nullptr));
			p.clipped = false;
			touched = true;
		}
	}
	return touched;
}

bool badge_scene::clear(group& g, std::vector<placed>& slot)
{
	for (const placed& p : slot) g.visual->RemoveVisual(p.visual.Get());
	const bool touched = !slot.empty();
	slot.clear();
	return touched;
}

IDCompositionSurface* badge_scene::sprite(const badge& b)
{
	const int size = sprite_size(b.diameter);
	auto& picture = sprites[{ b.number, size }];
	if (picture || broken) return picture.Get();

	const float d = static_cast<float>(size - 2);
	const std::wstring digit = std::to_wstring(b.number);
	IDWriteTextFormat* format = text_format(std::max(1, static_cast<int>(std::lround(d * 0.72f))));
	picture = dev.paint_surface(size, size, [&](ID2D1DeviceContext& ctx, D2D1_POINT_2F at) {
		ComPtr<ID2D1SolidColorBrush> red, white;
		ctx.CreateSolidColorBrush(badge_red, red.GetAddressOf());
		ctx.CreateSolidColorBrush(digit_white, white.GetAddressOf());

		const float c = size / 2.0f;
		ctx.FillEllipse(D2D1::Ellipse(D2D1::Point2F(at.x + c, at.y + c), d / 2, d / 2), red.Get());
		if (!format) return;
		ctx.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType needs an opaque background
		ctx.DrawText(digit.c_str(), static_cast<UINT32>(digit.size()), format,
			D2D1::RectF(at.x + 1, at.y + 1, at.x + 1 + d, at.y + 1 + d), white.Get());
	});
	broken = !picture;
	return picture.Get();
}

IDWriteTextFormat* badge_scene::text_format(int size)
{
	auto& format = formats[size];
	if (format) return format.Get();

	dev.dwrite().CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(size), L"en-us", format.GetAddressOf());
	if (format) {
		format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}
	return format.Get();
}
