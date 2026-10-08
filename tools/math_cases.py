"""Check the maths display conversion on the board (no AI calls): sends each sample with the
'$' developer command and compares the board's answer. Run: python tools/math_cases.py
The USB bridge must not be running (it holds the port)."""

import sys
import time

import serial
from serial.tools import list_ports

# (what a model wrote, what the screen should show)
CASES = [
    # Bracket delimiters: \( \) and \[ \]
    (r'The area is \(A = \pi r^2\).', 'The area is A = πr².'),
    (r'\[ x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a} \]', 'x = (−b ± √(b² − 4ac))/(2a)'),
    # Dollar delimiters: $ $ and $$ $$
    (r'So $x^{2} + y^{2} = r^{2}$ holds.', 'So x² + y² = r² holds.'),
    (r'$$\int_0^1 x^2\,dx = \frac{1}{3}$$', '∫₀¹ x² dx = ⅓'),
    (r'$\sum_{i=1}^{n} i = \frac{n(n+1)}{2}$', '∑ᵢ₌₁ⁿ i = (n(n + 1))/2'),
    (r'$\lim_{x \to 0} \frac{\sin x}{x} = 1$', 'lim(x→0) (sin x)/x = 1'),
    (r'$\theta = 30^\circ$ and $\Delta v \approx 9.8$', 'θ = 30° and Δv ≈ 9.8'),
    (r'$a_n = a_1 + (n-1)d$', 'aₙ = a₁ + (n − 1)d'),
    (r'$\mathbb{R}$, $x \in \mathbb{Z}$, $A \subseteq B$', 'ℝ, x ∈ ℤ, A ⊆ B'),
    (r'$\text{speed} = \frac{d}{t}$', 'speed = d/t'),
    (r'$e^{-x}$ and $10^{-3}$ and $x^{n+1}$', 'e⁻ˣ and 10⁻³ and xⁿ⁺¹'),
    (r'$\sqrt[3]{8} = 2$, $\frac{1}{2}mv^2$', '∛8 = 2, ½mv²'),
    (r"$f'(x) = 2x$, $\vec{F} = m\vec{a}$", 'f′(x) = 2x, F = ma'),
    (r'$x \neq 0$, $a \leq b \geq c$, $p \Rightarrow q$', 'x ≠ 0, a ≤ b ≥ c, p ⇒ q'),
    (r'$\begin{pmatrix} 1 & 2 \\ 3 & 4 \end{pmatrix}$', '(1, 2; 3, 4)'),
    (r'$O(n^2)$ vs $O(n \log n)$', 'O(n²) vs O(n log n)'),
    # money and code are not maths
    ('It costs $5 and $7 in total.', 'It costs $5 and $7 in total.'),
    ('Between $5-$7 each.', 'Between $5-$7 each.'),
    ('Use `x^2` in Python as `x**2`.', 'Use x^2 in Python as x**2.'),
    # bare LaTeX and plain-text maths
    (r'Answer: \frac{3}{4} of the total', 'Answer: ¾ of the total'),
    (r'Multiply 3 \times 4 = 12', 'Multiply 3 × 4 = 12'),
    ('x^2 + 2x + 1 = (x+1)^2', 'x² + 2x + 1 = (x + 1)²'),
    # ASCII maths from older answers
    ('7*(6*m+1)-1 = 42*m +7 -1', '7(6m + 1) − 1 = 42m + 7 − 1'),
    ('1. 3*4 = 12', '1. 3 × 4 = 12'),
    ('Total: 5*x + 2 = 17, so x = 3.', 'Total: 5x + 2 = 17, so x = 3.'),
    ('e^(x+1) and x^-1 and 2^0.5', 'eˣ⁺¹ and x⁻¹ and 2^(0.5)'),
    ('Released 2024-10-08 in the x-ray lab, step-by-step.', 'Released 2024-10-08 in the x-ray lab, step-by-step.'),
    ('- item one', '- item one'),
    ('**Bold** text and a * star', '**Bold** text and a * star'),
    ('sqrt(16) = 4, x <= 5, y != 3, a -> b', '√(16) = 4, x ≤ 5, y ≠ 3, a → b'),
    ('Already Unicode: x² + √9 = π', 'Already Unicode: x² + √9 = π'),
    ('**Answer:** 2^10 = 1024', '**Answer:** 2¹⁰ = 1024'),
    # streaming: an unfinished formula still reads sensibly
    (r'So \( x = \frac{1}{2', 'So x = ½'),
]

port = serial.Serial()
port.port = [p.device for p in list_ports.comports() if p.vid == 0x303A][0]
port.baudrate, port.timeout, port.dtr, port.rts = 115200, 0.1, True, False
port.open()
time.sleep(0.5)
port.reset_input_buffer()
failed = 0
for text, want in CASES:
    port.write(b'$' + text.encode('utf-8') + b'\n')
    end = time.time() + 10
    buf = b''
    got = None
    while time.time() < end:
        buf += port.read(4096)
        at = buf.find(b'MATH ')
        if at >= 0 and buf.find(b'\n', at) >= 0:  # the first line of the board's answer
            got = buf[at + 5 : buf.find(b'\n', at)].rstrip(b'\r').decode('utf-8', 'replace')
        if got is not None:
            break
    ok = got == want
    failed += not ok
    print(('PASS ' if ok else 'FAIL ') + text)
    if not ok:
        print('     got  ', got)
        print('     want ', want)
    time.sleep(0.1)
port.close()
print(f'{len(CASES) - failed}/{len(CASES)} passed')
sys.exit(1 if failed else 0)
