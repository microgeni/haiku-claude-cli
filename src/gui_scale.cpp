#include "gui_scale.h"

#include <cmath>

#include <Font.h>

namespace gui {

float Scale()
{
	const float base = be_plain_font ? be_plain_font->Size() : 12.0f;
	const float s    = base / 12.0f;
	// Clamp so an oddly configured font never produces an unusable window.
	if (s < 1.0f) return 1.0f;
	if (s > 4.0f) return 4.0f;
	return s;
}

float ScalePx(float px)
{
	return std::ceil(px * Scale());
}

} // namespace gui
