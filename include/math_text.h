#pragma once
#include <string>

// Answers as the screen can show them. Models write maths in different ways: GPT mostly
// \( \) and \[ \], Gemini mostly $ $ and $$ $$, every one of them bare \frac or
// x^2 at times, and plain Unicode (x², √, ≤). All of it becomes Unicode the display fonts
// draw: x^{2} -> x², \frac{a}{b} -> a/b, \sqrt{x} -> √x, \alpha -> α, \le -> ≤.
// Currency ("$5 and $7") and code (`...`, ``` blocks) are left alone. Used when text is
// shown, so answers saved before this existed are shown the new way too.
std::string mathToDisplay(const std::string &text);

// Keeps every character the display fonts can draw; folds the rest to near ASCII
// (or '?'), drops control characters other than newlines.
std::string foldToDisplay(const std::string &text);
