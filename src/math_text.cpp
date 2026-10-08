#include "math_text.h"

#include <ctype.h>
#include <string.h>

#include <vector>

#include "glyphs.h"

namespace {

// ---------- UTF-8 ----------
void put(std::string &out, uint32_t c) {
  if (c < 0x80) out += (char)c;
  else if (c < 0x800) {
    out += (char)(0xC0 | (c >> 6));
    out += (char)(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += (char)(0xE0 | (c >> 12));
    out += (char)(0x80 | ((c >> 6) & 0x3F));
    out += (char)(0x80 | (c & 0x3F));
  } else {
    out += (char)(0xF0 | (c >> 18));
    out += (char)(0x80 | ((c >> 12) & 0x3F));
    out += (char)(0x80 | ((c >> 6) & 0x3F));
    out += (char)(0x80 | (c & 0x3F));
  }
}
// The code point at s[i]; i moves past it. Broken sequences give U+FFFD.
uint32_t next(const std::string &s, size_t &i) {
  const uint8_t c = s[i];
  const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (c < 0x80) {
    ++i;
    return c;
  }
  if (!extra || i + extra >= s.size()) {
    ++i;
    return 0xFFFD;
  }
  uint32_t code = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : c & 0x1F;
  for (int k = 1; k <= extra; ++k) {
    const uint8_t b = s[i + k];
    if ((b & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    code = (code << 6) | (b & 0x3F);
  }
  i += extra + 1;
  return code;
}
std::vector<uint32_t> codes(const std::string &s) {
  std::vector<uint32_t> v;
  for (size_t i = 0; i < s.size();) v.push_back(next(s, i));
  return v;
}

// ---------- tables ----------
struct Sym {
  const char *name;
  uint32_t code;
};
const Sym SYMBOLS[] = {
    // Greek
    {"alpha", 0x3B1},
    {"beta", 0x3B2},
    {"gamma", 0x3B3},
    {"delta", 0x3B4},
    {"epsilon", 0x3B5},
    {"varepsilon", 0x3B5},
    {"zeta", 0x3B6},
    {"eta", 0x3B7},
    {"theta", 0x3B8},
    {"vartheta", 0x3D1},
    {"iota", 0x3B9},
    {"kappa", 0x3BA},
    {"lambda", 0x3BB},
    {"mu", 0x3BC},
    {"nu", 0x3BD},
    {"xi", 0x3BE},
    {"omicron", 0x3BF},
    {"pi", 0x3C0},
    {"varpi", 0x3C0},
    {"rho", 0x3C1},
    {"varrho", 0x3C1},
    {"sigma", 0x3C3},
    {"varsigma", 0x3C2},
    {"tau", 0x3C4},
    {"upsilon", 0x3C5},
    {"phi", 0x3C6},
    {"varphi", 0x3C6},
    {"chi", 0x3C7},
    {"psi", 0x3C8},
    {"omega", 0x3C9},
    {"Gamma", 0x393},
    {"Delta", 0x394},
    {"Theta", 0x398},
    {"Lambda", 0x39B},
    {"Xi", 0x39E},
    {"Pi", 0x3A0},
    {"Sigma", 0x3A3},
    {"Upsilon", 0x3A5},
    {"Phi", 0x3A6},
    {"Psi", 0x3A8},
    {"Omega", 0x3A9},
    // operators and relations
    {"times", 0xD7},
    {"cdot", 0xB7},
    {"cdotp", 0xB7},
    {"div", 0xF7},
    {"pm", 0xB1},
    {"mp", 0x2213},
    {"ast", '*'},
    {"star", '*'},
    {"circ", 0x2218},
    {"bullet", 0x2022},
    {"oplus", 0x2295},
    {"otimes", 0x2297},
    {"le", 0x2264},
    {"leq", 0x2264},
    {"leqslant", 0x2264},
    {"ge", 0x2265},
    {"geq", 0x2265},
    {"geqslant", 0x2265},
    {"ne", 0x2260},
    {"neq", 0x2260},
    {"approx", 0x2248},
    {"simeq", 0x2245},
    {"cong", 0x2245},
    {"equiv", 0x2261},
    {"sim", 0x223C},
    {"propto", 0x221D},
    {"ll", 0x226A},
    {"gg", 0x226B},
    {"lt", '<'},
    {"gt", '>'},
    {"infty", 0x221E},
    {"partial", 0x2202},
    {"nabla", 0x2207},
    {"forall", 0x2200},
    {"exists", 0x2203},
    {"emptyset", 0x2205},
    {"varnothing", 0x2205},
    {"in", 0x2208},
    {"notin", 0x2209},
    {"ni", 0x220B},
    {"subset", 0x2282},
    {"supset", 0x2283},
    {"subseteq", 0x2286},
    {"supseteq", 0x2287},
    {"cup", 0x222A},
    {"cap", 0x2229},
    {"setminus", '\\'},
    {"land", 0x2227},
    {"wedge", 0x2227},
    {"lor", 0x2228},
    {"vee", 0x2228},
    {"neg", 0xAC},
    {"lnot", 0xAC},
    {"angle", 0x2220},
    {"perp", 0x22A5},
    {"parallel", 0x2225},
    {"mid", '|'},
    {"therefore", 0x2234},
    {"because", 0x2235},
    {"sum", 0x2211},
    {"prod", 0x220F},
    {"int", 0x222B},
    {"iint", 0x222C},
    {"oint", 0x222E},
    {"surd", 0x221A},
    {"prime", 0x2032},
    {"degree", 0xB0},
    {"to", 0x2192},
    {"rightarrow", 0x2192},
    {"longrightarrow", 0x2192},
    {"leftarrow", 0x2190},
    {"gets", 0x2190},
    {"longleftarrow", 0x2190},
    {"leftrightarrow", 0x2194},
    {"uparrow", 0x2191},
    {"downarrow", 0x2193},
    {"Rightarrow", 0x21D2},
    {"implies", 0x21D2},
    {"Longrightarrow", 0x21D2},
    {"Leftarrow", 0x21D0},
    {"impliedby", 0x21D0},
    {"Leftrightarrow", 0x21D4},
    {"iff", 0x21D4},
    {"Longleftrightarrow", 0x21D4},
    {"mapsto", 0x2192},
    {"ldots", 0x2026},
    {"dots", 0x2026},
    {"cdots", 0x22EF},
    {"dotsb", 0x22EF},
    {"dotsc", 0x2026},
    {"vdots", 0x22EF},
    {"ddots", 0x22EF},
    {"hbar", 0x210F},
    {"ell", 0x2113},
    {"lfloor", 0x230A},
    {"rfloor", 0x230B},
    {"lceil", 0x2308},
    {"rceil", 0x2309},
    {"langle", '<'},
    {"rangle", '>'},
    {"lvert", '|'},
    {"rvert", '|'},
    {"vert", '|'},
    {"lVert", 0x2016},
    {"rVert", 0x2016},
    {"Vert", 0x2016},
    {"backslash", '\\'},
    {"checkmark", 0x2713},
    {"%", '%'},
    {"euro", 0x20AC},
    {"cdotscalar", 0xB7},
    {"quad", ' '},
    {"qquad", ' '},
    {"space", ' '},
    {"enspace", ' '},
    {"thinspace", ' '},
};
// Names written as words (functions): shown as the word.
const char *const WORDS[] = {"sin",  "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
                             "tanh", "log", "ln",  "lg",  "exp", "lim", "max",    "min",    "sup",    "inf",  "det",
                             "gcd",  "lcm", "deg", "dim", "ker", "arg", "Pr",     "mod",    "bmod",   "pmod"};
// Commands that only change how the letters look: keep the letters.
const char *const STYLES[] = {"mathbf",    "mathit",    "mathsf",       "mathtt",    "boldsymbol",     "bm",
                              "vec",       "hat",       "widehat",      "bar",       "overline",       "underline",
                              "tilde",     "widetilde", "dot",          "ddot",      "overrightarrow", "mathcal",
                              "mathscr",   "mathfrak",  "displaystyle", "textstyle", "scriptstyle",    "underbrace",
                              "overbrace", "cancel",    "boxed",        "pmb"};
// Commands with words inside: keep the words as written.
const char *const TEXTS[] = {"text",   "textrm", "textbf", "textit",       "textsf", "texttt",    "textnormal",
                             "mathrm", "mbox",   "hbox",   "operatorname", "emph",   "mathnormal"};
// Sizing and spacing that has no meaning on this screen.
const char *const IGNORED[] = {"left",   "right",     "big",      "Big",      "bigg",  "Bigg",  "bigl",
                               "bigr",   "Bigl",      "Bigr",     "biggl",    "biggr", "Biggl", "Biggr",
                               "middle", "limits",    "nolimits", "nonumber", "notag", "label", "tag",
                               "hline",  "centering", "noindent", "small",    "large"};

struct Pair {
  uint32_t from, to;
};
const Pair SUPER[] = {
    {'0', 0x2070}, {'1', 0xB9},   {'2', 0xB2},   {'3', 0xB3},   {'4', 0x2074}, {'5', 0x2075},    {'6', 0x2076},
    {'7', 0x2077}, {'8', 0x2078}, {'9', 0x2079}, {'+', 0x207A}, {'-', 0x207B}, {0x2212, 0x207B}, {'=', 0x207C},
    {'(', 0x207D}, {')', 0x207E}, {'n', 0x207F}, {'i', 0x2071}, {'a', 0x1D43}, {'b', 0x1D47},    {'c', 0x1D9C},
    {'d', 0x1D48}, {'e', 0x1D49}, {'f', 0x1DA0}, {'g', 0x1D4D}, {'h', 0x2B0},  {'j', 0x2B2},     {'k', 0x1D4F},
    {'l', 0x2E1},  {'m', 0x1D50}, {'o', 0x1D52}, {'p', 0x1D56}, {'r', 0x2B3},  {'s', 0x2E2},     {'t', 0x1D57},
    {'u', 0x1D58}, {'v', 0x1D5B}, {'w', 0x2B7},  {'x', 0x2E3},  {'y', 0x2B8},  {'z', 0x1DBB},    {'T', 0x1D40},
};
const Pair SUB[] = {
    {'0', 0x2080}, {'1', 0x2081}, {'2', 0x2082}, {'3', 0x2083}, {'4', 0x2084}, {'5', 0x2085},    {'6', 0x2086},
    {'7', 0x2087}, {'8', 0x2088}, {'9', 0x2089}, {'+', 0x208A}, {'-', 0x208B}, {0x2212, 0x208B}, {'=', 0x208C},
    {'(', 0x208D}, {')', 0x208E}, {'a', 0x2090}, {'e', 0x2091}, {'o', 0x2092}, {'x', 0x2093},    {'h', 0x2095},
    {'k', 0x2096}, {'l', 0x2097}, {'m', 0x2098}, {'n', 0x2099}, {'p', 0x209A}, {'s', 0x209B},    {'t', 0x209C},
    {'i', 0x1D62}, {'r', 0x1D63}, {'u', 0x1D64}, {'v', 0x1D65}, {'j', 0x2C7C},
};
// Already-raised characters stay raised when they meet ^ again (x^² is rare but harmless).
const Pair FRACTIONS[] = {{12, 0xBD},   {13, 0x2153}, {23, 0x2154}, {14, 0xBC},   {34, 0xBE},
                          {15, 0x2155}, {25, 0x2156}, {35, 0x2157}, {45, 0x2158}, {16, 0x2159},
                          {56, 0x215A}, {18, 0x215B}, {38, 0x215C}, {58, 0x215D}, {78, 0x215E}};

template <size_t N>
bool inList(const char *const (&list)[N], const std::string &name) {
  for (const char *w : list)
    if (name == w) return true;
  return false;
}
bool symbol(const std::string &name, uint32_t &code) {
  for (const Sym &s : SYMBOLS)
    if (name == s.name) {
      code = s.code;
      return true;
    }
  return false;
}
template <size_t N>
bool mapAll(const Pair (&table)[N], const std::string &in, std::string &out) {
  out.clear();
  for (uint32_t c : codes(in)) {
    if (c == ' ') continue;
    bool found = false;
    for (const Pair &p : table)
      if (p.from == c || p.to == c) {
        put(out, p.to);
        found = true;
        break;
      }
    if (!found) return false;
  }
  return !out.empty();
}
uint32_t blackboard(char c) {
  switch (c) {
    case 'R': return 0x211D;
    case 'N': return 0x2115;
    case 'Z': return 0x2124;
    case 'Q': return 0x211A;
    case 'C': return 0x2102;
    default: return (uint8_t)c;
  }
}
bool scriptChar(uint32_t c) {
  for (const Pair &p : SUPER)
    if (p.to == c) return true;
  for (const Pair &p : SUB)
    if (p.to == c) return true;
  return c == 0x2032 || c == 0x2033;
}
// Spaced in maths, as TeX does: relations and binary operators.
bool spacedOp(uint32_t c) {
  static const uint32_t OPS[] = {'=',    '<',    '>',    '+',    0x2212, 0xB1,   0x2213, 0xD7,   0xF7,
                                 0x2264, 0x2265, 0x2260, 0x2248, 0x2261, 0x223C, 0x2245, 0x221D, 0x2192,
                                 0x2190, 0x2194, 0x21D2, 0x21D0, 0x21D4, 0x2208, 0x2209, 0x220B, 0x2282,
                                 0x2283, 0x2286, 0x2287, 0x222A, 0x2229, 0x2227, 0x2228, 0x226A, 0x226B};
  for (uint32_t o : OPS)
    if (o == c) return true;
  return false;
}
// A piece that reads unambiguously without brackets: 12, 3.5, x, α, x², aₙ, dx, ∂y.
bool atom(const std::string &s) {
  const std::vector<uint32_t> v = codes(s);
  if (v.empty()) return true;
  bool number = true;
  for (uint32_t c : v)
    if (!(c < 0x80 && (isdigit((int)c) || c == '.'))) number = false;
  if (number) return true;
  size_t k = 1;
  if ((v[0] == 'd' || v[0] == 0x2202) && v.size() >= 2 && v[1] < 0x80 && isalpha((int)v[1])) k = 2;  // dx, ∂y
  if (v[0] < 0x80 && !isalnum((int)v[0]) && v[0] != 0x2202) return false;
  if (v[0] >= 0x80 && !(v[0] >= 0x370 && v[0] < 0x400) && v[0] != 0x2202 && v[0] != 0x221E) return false;
  for (; k < v.size(); ++k)
    if (!scriptChar(v[k])) return false;
  return true;
}
std::string bracket(const std::string &s) { return atom(s) ? s : "(" + s + ")"; }
void trimSpaces(std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && s[a] == ' ') ++a;
  while (b > a && s[b - 1] == ' ') --b;
  s = s.substr(a, b - a);
}

// ---------- LaTeX maths -> Unicode ----------
struct Math {
  const std::string &s;
  size_t i = 0;
  std::vector<std::string> envs;  // \begin{...} stack (matrices write rows differently)
  bool spaceNext = false;         // after sin, log, ∑, ∫: a space before the next letter
  explicit Math(const std::string &src) : s(src) {}

  static uint32_t lastCode(const std::string &out) {
    size_t k = out.size();
    while (k > 0 && out[k - 1] == ' ') --k;
    if (!k) return 0;
    size_t start = k - 1;
    while (start > 0 && ((uint8_t)out[start] & 0xC0) == 0x80) --start;
    size_t j = start;
    return next(out, j);
  }
  static void trimEnd(std::string &out) {
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  // Source spaces mean nothing in maths; spacing comes from what the pieces are.
  void emit(std::string &out, const std::string &t) {
    if (t.empty()) return;
    if (spaceNext) {
      size_t j = 0;
      const uint32_t c = next(t, j);
      if ((c < 0x80 && isalnum((int)c)) || (c >= 0x370 && c < 0x400) || c == 0x221A || c == 0x221B)
        if (!out.empty() && out.back() != ' ' && out.back() != '\n') out += ' ';
      spaceNext = false;
    }
    out += t;
  }
  void op(std::string &out, uint32_t code) {
    spaceNext = false;
    const uint32_t prev = lastCode(out);
    const bool unary = (code == '+' || code == 0x2212 || code == 0xB1 || code == 0x2213) &&
                       (!prev || prev == '(' || prev == '[' || prev == '{' || prev == ',' || prev == ';' ||
                        prev == '|' || prev == '\n' || spacedOp(prev));
    trimEnd(out);
    if (unary) {
      if (!out.empty() && (spacedOp(prev) || prev == ',' || prev == ';')) out += ' ';
      put(out, code);
      return;
    }
    if (!out.empty() && out.back() != '\n') out += ' ';
    put(out, code);
    out += ' ';
  }
  void word(std::string &out, const std::string &w) {
    const uint32_t prev = lastCode(out);
    if (!out.empty() && out.back() != ' ' && ((prev < 0x80 && isalnum((int)prev)) || scriptChar(prev))) out += ' ';
    spaceNext = false;
    out += w;
    spaceNext = true;
  }

  bool inMatrix() const {
    for (const std::string &e : envs)
      if (e.find("matrix") != std::string::npos || e == "array") return true;
    return false;
  }
  void skipSpaces() {
    while (i < s.size() && s[i] == ' ') ++i;
  }
  // {...} contents (without the braces); i moves past the closing brace.
  std::string braced() {
    int depth = 0;
    const size_t start = i + 1;
    for (; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        ++i;
        continue;
      }
      if (s[i] == '{') ++depth;
      else if (s[i] == '}' && --depth == 0) {
        ++i;
        return s.substr(start, i - 1 - start);
      }
    }
    return s.substr(start < s.size() ? start : s.size());  // unclosed (still streaming)
  }
  // One argument, raw: {group}, \command, or one character.
  std::string rawArg() {
    skipSpaces();
    if (i >= s.size()) return "";
    if (s[i] == '{') return braced();
    if (s[i] == '\\') {
      const size_t start = i++;
      if (i < s.size() && isalpha((uint8_t)s[i]))
        while (i < s.size() && isalpha((uint8_t)s[i])) ++i;
      else if (i < s.size()) ++i;
      return s.substr(start, i - start);
    }
    const size_t start = i;
    next(s, i);
    return s.substr(start, i - start);
  }
  std::string arg() { return convert(rawArg()); }
  static std::string convert(const std::string &raw) {
    Math m(raw);
    std::string out = m.run();
    trimSpaces(out);
    return out;
  }
  static bool endsWithWord(const std::string &out, const char *w) {
    const size_t n = strlen(w);
    return out.size() >= n && out.compare(out.size() - n, n, w) == 0 &&
           (out.size() == n || !isalpha((uint8_t)out[out.size() - n - 1]));
  }
  void script(std::string &out, bool up) {
    std::string raw = rawArg();
    if (up && (raw == "\\circ" || raw == "o" || raw == "\\degree")) {
      put(out, 0xB0);
      return;
    }
    if (up && (raw == "\\prime" || raw == "'")) {
      put(out, 0x2032);
      return;
    }
    if (up && raw == "\\prime\\prime") {
      put(out, 0x2033);
      return;
    }
    const std::string g = convert(raw);
    std::string mapped;
    if (up ? mapAll(SUPER, g, mapped) : mapAll(SUB, g, mapped)) {
      out += mapped;
      return;
    }
    std::string tight;
    for (char ch : g)
      if (ch != ' ') tight += ch;
    // Limits under lim/max/min read best in brackets: lim(x→0).
    for (const char *w : {"lim", "max", "min", "sup", "inf"})
      if (!up && endsWithWord(out, w)) {
        out += "(" + tight + ") ";
        spaceNext = false;
        return;
      }
    out += up ? "^" : "_";
    out += (codes(tight).size() == 1) ? tight : "(" + tight + ")";
  }
  void command(std::string &out) {
    ++i;  // the backslash
    if (i >= s.size()) return;
    if (!isalpha((uint8_t)s[i])) {
      const char c = s[i++];
      if (c == '\\') {
        trimEnd(out);
        out += inMatrix() ? "; " : "\n";
      } else if (c == ',' || c == ':' || c == ';' || c == ' ' || c == '>') {
        if (!out.empty() && out.back() != ' ') out += ' ';
      } else if (c == '!' || c == '/') {
      } else if (c == '|') put(out, 0x2016);
      else emit(out, std::string(1, c));  // \{ \} \% \$ \_ \# \&
      return;
    }
    const size_t start = i;
    while (i < s.size() && isalpha((uint8_t)s[i])) ++i;
    const std::string name = s.substr(start, i - start);
    uint32_t code;
    if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "cfrac") {
      const std::string a = arg(), b = arg();
      const bool digits = a.size() == 1 && b.size() == 1 && isdigit((uint8_t)a[0]) && isdigit((uint8_t)b[0]);
      if (digits) {
        const int key = (a[0] - '0') * 10 + (b[0] - '0');
        for (const Pair &f : FRACTIONS)
          if ((int)f.from == key) {
            std::string v;
            put(v, f.to);
            emit(out, v);
            return;
          }
      }
      emit(out, bracket(a) + "/" + bracket(b));
    } else if (name == "binom" || name == "dbinom" || name == "tbinom") {
      const std::string n = arg(), k = arg();
      emit(out, "C(" + n + ", " + k + ")");
    } else if (name == "sqrt") {
      std::string index;
      skipSpaces();
      if (i < s.size() && s[i] == '[') {
        const size_t close = s.find(']', i);
        index = convert(s.substr(i + 1, (close == std::string::npos ? s.size() : close) - i - 1));
        i = close == std::string::npos ? s.size() : close + 1;
      }
      const std::string x = arg();
      std::string v;
      if (index == "3") put(v, 0x221B);
      else {
        std::string raised;
        if (!index.empty()) v += mapAll(SUPER, index, raised) ? raised : "(" + index + ")";
        put(v, 0x221A);
      }
      emit(out, v + bracket(x));
    } else if (inList(TEXTS, name)) {
      skipSpaces();
      std::string t = i < s.size() && s[i] == '{' ? braced() : rawArg();
      for (size_t k = 0; k + 1 < t.size(); ++k)
        if (t[k] == '\\' && !isalpha((uint8_t)t[k + 1])) t.erase(k, 1);  // \% \$ \_ inside text
      if (name == "operatorname") {
        word(out, t);
        return;
      }
      // Words keep their own spacing; one space separates them from maths on either side.
      if (!out.empty() && out.back() != ' ' && out.back() != '\n' && !t.empty() && t[0] != ' ' && t[0] != ',' &&
          t[0] != '.')
        out += ' ';
      spaceNext = false;
      out += t;
      if (!t.empty() && t.back() != ' ') spaceNext = true;
    } else if (name == "mathbb" || name == "Bbb") {
      const std::string x = rawArg();
      std::string v;
      if (x.size() == 1) put(v, blackboard(x[0]));
      else v = convert(x);
      emit(out, v);
    } else if (inList(STYLES, name)) {
      if (name == "displaystyle" || name == "textstyle" || name == "scriptstyle") return;
      emit(out, arg());
    } else if (name == "begin" || name == "end") {
      const std::string env = rawArg();
      if (name == "begin") {
        envs.push_back(env);
        if (env == "array") rawArg();  // column spec
        if (env.find("matrix") != std::string::npos) emit(out, env[0] == 'v' ? "|" : env[0] == 'b' ? "[" : "(");
        else if (env == "cases") emit(out, "{ ");
      } else {
        if (env.find("matrix") != std::string::npos) {
          trimEnd(out);
          out += env[0] == 'v' ? "|" : env[0] == 'b' ? "]" : ")";
        }
        if (!envs.empty()) envs.pop_back();
      }
    } else if (inList(IGNORED, name)) {
      if (name == "label" || name == "tag") rawArg();
      skipSpaces();
      if ((name == "left" || name == "right") && i < s.size() && s[i] == '.') ++i;
    } else if (name == "not") {
      skipSpaces();
      if (i < s.size() && s[i] == '=') {
        ++i;
        op(out, 0x2260);
      } else if (s.compare(i, 3, "\\in") == 0 && (i + 3 >= s.size() || !isalpha((uint8_t)s[i + 3]))) {
        i += 3;
        op(out, 0x2209);
      }
    } else if (name == "pmod" || name == "bmod" || name == "mod") {
      if (name == "pmod") {
        trimEnd(out);
        out += " (mod " + arg() + ")";
      } else {
        trimEnd(out);
        out += " mod ";
        spaceNext = false;
      }
    } else if (symbol(name, code)) {
      if (spacedOp(code)) {
        op(out, code);
        return;
      }
      if (code == ' ') {
        if (!out.empty() && out.back() != ' ') out += ' ';
        return;
      }
      std::string v;
      put(v, code);
      emit(out, v);
      if (code == 0x2211 || code == 0x220F || code == 0x222B || code == 0x222C || code == 0x222E) spaceNext = true;
    } else {
      word(out, name);  // \sin, \log, \max and anything unknown: the word itself
    }
  }
  std::string run() {
    std::string out;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '\\') command(out);
      else if (c == '^') {
        ++i;
        script(out, true);
      } else if (c == '_') {
        ++i;
        script(out, false);
      } else if (c == '{') emit(out, convert(braced()));
      else if (c == '}') ++i;
      else if (c == '&') {
        ++i;
        trimEnd(out);
        out += inMatrix() ? ", " : " ";
      } else if (c == '~') {
        ++i;
        if (!out.empty() && out.back() != ' ') out += ' ';
      } else if (c == '\'') {
        ++i;
        put(out, 0x2032);
      } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') ++i;  // spacing comes from the pieces
      else if (c == '=' || c == '+' || c == '-' || c == '<' || c == '>') {
        ++i;
        op(out, c == '-' ? 0x2212 : (uint8_t)c);
      } else if (c == ',' || c == ';') {
        ++i;
        trimEnd(out);
        out += c;
        out += ' ';
        spaceNext = false;
      } else {
        const size_t start = i;
        const uint32_t code = next(s, i);
        if (spacedOp(code)) op(out, code);  // Unicode written inside maths: ≤, ×, →
        else emit(out, s.substr(start, i - start));
      }
    }
    // Collapse runs of spaces.
    std::string tidy;
    for (char ch : out)
      if (!(ch == ' ' && !tidy.empty() && (tidy.back() == ' ' || tidy.back() == '\n'))) tidy += ch;
    return tidy;
  }
};

// ---------- plain text around the maths ----------
bool startsWith(const std::string &s, size_t i, const char *p) { return s.compare(i, strlen(p), p) == 0; }
size_t findFrom(const std::string &s, size_t i, const char *p) { return s.find(p, i); }

// A known command outside any delimiters (\frac{1}{2}, \times, \alpha): worth converting.
bool knownCommand(const std::string &s, size_t i) {
  if (i + 1 >= s.size() || !isalpha((uint8_t)s[i + 1])) return false;
  size_t e = i + 1;
  while (e < s.size() && isalpha((uint8_t)s[e])) ++e;
  const std::string name = s.substr(i + 1, e - i - 1);
  uint32_t code;
  return symbol(name, code) || inList(WORDS, name) || inList(TEXTS, name) || inList(STYLES, name) || name == "frac" ||
         name == "dfrac" || name == "tfrac" || name == "sqrt" || name == "binom" || name == "mathbb" ||
         name == "left" || name == "right" || name == "begin" || name == "end";
}
// The extent of a bare command and its arguments: up to the next space that is not inside braces.
size_t bareEnd(const std::string &s, size_t i) {
  int depth = 0;
  size_t e = i + 1;
  while (e < s.size() && isalpha((uint8_t)s[e])) ++e;
  for (; e < s.size(); ++e) {
    const char c = s[e];
    if (c == '{') ++depth;
    else if (c == '}') {
      if (--depth < 0) break;
    } else if (depth == 0 && (c == ' ' || c == '\n' || c == ',' || c == '.' || c == ';' || c == ')')) {
      // keep going through attached scripts and further commands: \frac{a}{b}^2
      break;
    } else if (depth == 0 && !(c == '^' || c == '_' || c == '[' || c == ']' || c == '\\' || isalnum((uint8_t)c))) break;
  }
  return e;
}
// "x^2", "e^(-x)", "10^-3", "x^{n+1}" in ordinary text.
bool plainScript(const std::string &s, size_t &i, std::string &out) {
  if (out.empty() || out.back() == ' ' || out.back() == '\n') return false;
  size_t e = i + 1;
  std::string g;
  if (e < s.size() && (s[e] == '{' || s[e] == '(')) {
    const char close = s[e] == '{' ? '}' : ')';
    const size_t c = s.find(close, e);
    if (c == std::string::npos || c - e > 12) return false;
    g = Math::convert(s.substr(e + 1, c - e - 1));
    e = c + 1;
  } else {
    const size_t start = e;
    if (e < s.size() && (s[e] == '-' || s[e] == '+')) ++e;
    while (e < s.size() && isalnum((uint8_t)s[e]) && e - start < 6) ++e;
    g = s.substr(start, e - start);
    // a word after ^ (x^abc) is probably not an exponent beyond its first letter
    bool digits = true;
    for (char ch : g)
      if (!(isdigit((uint8_t)ch) || ch == '-' || ch == '+')) digits = false;
    if (!digits && g.size() > 1 && !(g.size() == 2 && (g[0] == '-' || g[0] == '+'))) {
      g = g.substr(0, 1);
      e = start + 1;
    }
  }
  std::string mapped;
  if (g.empty() || !mapAll(SUPER, g, mapped)) return false;
  out += mapped;
  i = e;
  return true;
}
const struct {
  const char *from;
  uint32_t to;
} ASCII_MATH[] = {
    {"<=>", 0x21D4}, {"<=", 0x2264}, {">=", 0x2265}, {"!=", 0x2260}, {"=/=", 0x2260},   {"+/-", 0xB1},
    {"->", 0x2192},  {"<-", 0x2190}, {"=>", 0x21D2}, {"~=", 0x2248}, {"sqrt(", 0x221A},
};

}  // namespace

std::string mathToDisplay(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 16);
  bool fence = false;
  size_t i = 0;
  auto block = [&](const std::string &inner) {
    // Display maths: its own line, unless the line so far is only a step number or bullet
    // ("3. $$...$$" stays one step).
    const size_t ls = out.rfind('\n') == std::string::npos ? 0 : out.rfind('\n') + 1;
    std::string lead = out.substr(ls);
    trimSpaces(lead);
    bool marker = lead.empty() || lead == "-" || lead == "*" || lead == "\xE2\x80\xA2";
    if (!marker && lead.size() <= 4 && (lead.back() == '.' || lead.back() == ')')) {
      marker = true;
      for (size_t k = 0; k + 1 < lead.size(); ++k)
        if (!isdigit((uint8_t)lead[k])) marker = false;
    }
    if (marker) {
      if (!lead.empty() && out.back() != ' ') out += ' ';
    } else if (!out.empty() && out.back() != '\n') out += '\n';
    out += Math::convert(inner);
    out += '\n';
  };
  while (i < s.size()) {
    const bool lineStart = i == 0 || s[i - 1] == '\n';
    if (lineStart && startsWith(s, i, "```")) {
      fence = !fence;
      const size_t eol = s.find('\n', i);
      i = eol == std::string::npos ? s.size() : eol + 1;  // the fence line itself is not shown
      continue;
    }
    if (fence) {
      out += s[i++];
      continue;
    }
    const char c = s[i];
    if (c == '`') {  // inline code: as written, without the backticks
      const size_t close = s.find('`', i + 1);
      const size_t eol = s.find('\n', i + 1);
      if (close != std::string::npos && (eol == std::string::npos || close < eol)) {
        out.append(s, i + 1, close - i - 1);
        i = close + 1;
      } else ++i;
      continue;
    }
    if (startsWith(s, i, "$$") || startsWith(s, i, "\\[")) {
      const char *closing = s[i] == '$' ? "$$" : "\\]";
      const size_t close = findFrom(s, i + 2, closing);
      block(s.substr(i + 2, (close == std::string::npos ? s.size() : close) - i - 2));  // unclosed: streaming
      i = close == std::string::npos ? s.size() : close + 2;
      while (i < s.size() && (s[i] == ' ')) ++i;
      if (i < s.size() && s[i] == '\n') ++i;
      continue;
    }
    if (startsWith(s, i, "\\(")) {
      const size_t close = findFrom(s, i + 2, "\\)");
      out += Math::convert(s.substr(i + 2, (close == std::string::npos ? s.size() : close) - i - 2));
      i = close == std::string::npos ? s.size() : close + 2;
      continue;
    }
    if (c == '$') {
      // Inline maths only when it cannot be money: "$x^2$" yes; "$5 and $7", "$5-$7" no
      // (Pandoc's rule: no space inside either dollar, no digit right after the closing one).
      size_t close = std::string::npos;
      if (i + 1 < s.size() && s[i + 1] != ' ' && s[i + 1] != '\n') {
        for (size_t k = i + 1; k < s.size() && s[k] != '\n'; ++k) {
          if (s[k] == '\\') {
            ++k;
            continue;
          }
          if (s[k] == '$') {
            close = k;
            break;
          }
        }
      }
      if (close != std::string::npos && s[close - 1] != ' ' &&
          !(close + 1 < s.size() && isdigit((uint8_t)s[close + 1]))) {
        out += Math::convert(s.substr(i + 1, close - i - 1));
        i = close + 1;
        continue;
      }
      out += c;
      ++i;
      continue;
    }
    if (c == '\\') {
      if (knownCommand(s, i)) {
        const size_t e = bareEnd(s, i);
        out += Math::convert(s.substr(i, e - i));
        i = e;
        continue;
      }
      if (i + 1 < s.size() && strchr("*_#`[]()-.!+{}|>%&$", s[i + 1])) {  // Markdown escapes
        out += s[i + 1];
        i += 2;
        continue;
      }
    }
    if (c == '^' && plainScript(s, i, out)) continue;
    bool replaced = false;
    for (const auto &m : ASCII_MATH) {
      if (startsWith(s, i, m.from)) {
        // "->" inside "-->" or "<!--": leave odd runs alone
        if (m.to == 0x2192 && i > 0 && s[i - 1] == '-') break;
        put(out, m.to);
        i += strlen(m.from);
        if (m.to == 0x221A) out += '(';
        replaced = true;
        break;
      }
    }
    if (replaced) continue;
    out += s[i++];
  }
  return foldToDisplay(out);
}

std::string foldToDisplay(const std::string &s) {
  static const struct {
    uint32_t code;
    const char *text;
  } FOLD[] = {
      {0x00A0, " "},   {0x2009, " "},  {0x200A, " "},  {0x202F, " "},  {0x2002, " "},  {0x2003, " "},  {0x200B, ""},
      {0x200C, ""},    {0x200D, ""},   {0xFEFF, ""},   {0x2010, "-"},  {0x2011, "-"},  {0x2012, "-"},  {0x2015, "-"},
      {0x2215, "/"},   {0x2223, "|"},  {0x2236, ":"},  {0x22C6, "*"},  {0x2217, "*"},  {0x27E8, "<"},  {0x27E9, ">"},
      {0x2329, "<"},   {0x232A, ">"},  {0x3008, "<"},  {0x3009, ">"},  {0x21A6, "->"}, {0x27F6, "->"}, {0x27F9, "=>"},
      {0x27FA, "<=>"}, {0x2A7D, "<="}, {0x2A7E, ">="}, {0x2266, "<="}, {0x2267, ">="}, {0x2254, ":="}, {0x2034, "'''"},
      {0x2016, "||"},  {0x2003, " "},  {0x201E, "\""}, {0x201A, "'"},  {0x2039, "<"},  {0x203A, ">"},  {0x2715, "x"},
      {0x2716, "x"},   {0x2714, "v"},  {0x2705, "v"},  {0x274C, "x"},  {0x26A0, "!"},  {0x2B50, "*"},  {0x1F4A1, ""},
      {0x1F449, "->"},
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const uint32_t c = next(s, i);
    if (c == '\n') {
      out += '\n';
      continue;
    }
    if (c == '\t') {
      out += ' ';
      continue;
    }
    if (c < 0x20 || c == 0x7F) continue;
    if (displayHasGlyph(c)) {
      put(out, c);
      continue;
    }
    const char *text = nullptr;
    for (const auto &f : FOLD)
      if (f.code == c) {
        text = f.text;
        break;
      }
    if (!text && c >= 0x1D400 && c <= 0x1D7FF) {  // mathematical bold/italic letters
      const uint32_t k = (c - 0x1D400) % 52;
      put(out, k < 26 ? 'A' + k : 'a' + k - 26);
      continue;
    }
    if (!text && c >= 0x1F000) continue;  // emoji: nothing to draw them with
    out += text ? text : "?";
  }
  return out;
}
