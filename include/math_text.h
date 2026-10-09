#pragma once
#include <string>

// Converts the maths in an answer to Unicode the display fonts can draw. Handles LaTeX in
// any delimiter style (\( \), \[ \], $ $, $$ $$, or none), ASCII forms such as x^2 and 7*x,
// and text that is already Unicode: x^{2} -> x², \frac{a}{b} -> a/b, \sqrt{x} -> √x, \le -> ≤.
// Currency ("$5 and $7") and code spans are left alone. Applied at display time, so stored
// answers are not modified.
std::string mathToDisplay(const std::string &text);

// Keeps every character the display fonts can draw; folds the rest to near ASCII
// (or '?'), drops control characters other than newlines.
std::string foldToDisplay(const std::string &text);
