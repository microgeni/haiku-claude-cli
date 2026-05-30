#pragma once
// Minimal forward declaration and wrapper for BScintillaView, which ships
// as libscintilla.so on Haiku but has no public header in the package.
// We only need the constructor + SendMessage + SetText + Show/Hide.

#include <View.h>

class BScintillaView : public BView {
public:
	BScintillaView(const char* name, uint32 flags,
	               bool horizontal, bool vertical,
	               border_style border = B_NO_BORDER);
	virtual ~BScintillaView();

	// Raw SCI_* message dispatch — same as Scintilla's ScintillaCall.
	long SendMessage(unsigned int iMessage, unsigned long wParam, long lParam);

	// Convenience text accessors.
	void SetText(const char* text);
	void GetText(int start, int end, char* out);
	int  TextLength();

	virtual void NotificationReceived(struct SCNotification* notification);
	virtual void ContextMenu(BPoint point);

private:
	// Prevent accidental copies of this heavyweight widget.
	BScintillaView(const BScintillaView&) = delete;
	BScintillaView& operator=(const BScintillaView&) = delete;
};
