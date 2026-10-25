# bc: the calculator

LakeShark has a calculator in the syntax of the Unix `bc` tool, in two places:

- **Console:** `bc 2^10` prints `1024`. The arguments are joined with spaces,
  so `bc 2 * 3` works; quote anything with a `;` in it (`bc "scale=2; 1/3"`).
  `bc` on its own starts a `bc>` session on the serial console, with the same
  line editing and Up-arrow history as the `lakeshark>` prompt; `quit`, `q`,
  `exit`, Ctrl-D or Ctrl-C leaves it, and so does Esc then Enter on an empty
  line. On a terminal that does not answer escape sequences (the USB-serial
  console usually), the session reads the line itself, without editing or
  history, so that Ctrl-C and Ctrl-D still reach it. From the TERMINAL app only the
  one-expression form works: a session there would be reading the serial
  port, not the board's keyboard. Use the BC app instead.
- **BC app**, in SYSTEM on the home screen. See docs/UI_CONTROLS.md for keys
  and the touch keypad.

Each keeps its own variables, `scale`, `ibase`, `obase` and `last` until the
board restarts (the console's survive between `bc` commands). Nothing is
saved to flash.

Built with `CONFIG_LS_BC` (on by default; about 16 KB of flash). The engine is
`components/lsbc`, tested on the host by `bench/tests/test_bc.c`.

## What it understands

```
bc> scale=2; 2/3
.66
bc> r = 1.5
bc> 3.14159 * r^2
7.06857
bc> obase=16; 255
FF
bc> obase=10; sqrt(2)
1.41
bc> 1/0
1/0
 ^
divide by zero at column 2
```

| | |
|---|---|
| Numbers | `12`, `3.5`, `.5`; in `ibase=16`, `FF` and `1.8` |
| Arithmetic | `+ - * / % ^`, unary `-`, parentheses. `^` is right associative and wants an integer exponent, negative allowed (`2^-2`) |
| Comparison | `< <= > >= == !=`, giving 1 or 0 |
| Logic | `!`, `&&`, `\|\|`, giving 1 or 0; `&&` and `\|\|` stop as soon as the answer is known |
| Variables | lower-case letters, digits and `_`, starting with a letter, up to 15 characters; 0 until assigned; 32 of them |
| Assignment | `=  +=  -=  *=  /=  %=  ^=`, and `++` / `--` before or after a name |
| Special variables | `scale` (digits after the point, 0-20), `ibase` (10 or 16), `obase` (2, 8, 10 or 16), `last` (the last value printed; `.` alone means the same) |
| Functions | `sqrt(x)`, `length(x)` (significant digits), `scale(x)` (digits after the point), and the `bc -l` set: `s(x)` sine, `c(x)` cosine, `a(x)` arctangent, `l(x)` natural log, `e(x)` exponential, all in radians. `abs(x)` is an extension |
| Statements | separated by `;` or new lines. An expression prints its value; an assignment prints nothing (put it in parentheses to see it) |
| Comments | `/* ... */` and `#` to the end of the line |
| Leaving | `quit`, `q`, `exit` or `halt` on its own |

Precedence, loosest first, is bc's: `||`, `&&`, comparison, assignment,
`+ -`, `* / %`, `^`, unary `-` and `!`, then `++ --`. Two consequences
surprise people and are what bc does too:

- `-2^2` is `4`: the minus belongs to the 2. Write `-(2^2)` or `0-2^2` for -4.
- `a = 3 < 5` assigns 3 to `a` and prints 1, because `=` binds tighter than
  `<`.

### How answers are printed

As bc does. Each value carries a number of digits after the point: a number
you type has as many as you wrote, `/`, `sqrt` and the `-l` functions give
`scale` digits, `+` and `-` keep the larger of their two, and `*` and `^`
follow bc's rules (`.5*.5` is `.2` at scale 0). The answer is cut, not
rounded, to those digits, and trailing zeros are kept: `2/3` at scale 2 is
`.66`, `4/2` at scale 6 is `2.000000`, `7.10` prints as `7.10`. A number
below 1 has no leading zero (`.5`), and zero is always `0`.

Because every step is cut to its scale, `7/2*2` is `6` at `scale=0`, which is
the old bc way of doing integer division.

In other output bases the integer part is exact and the fraction gets as
many digits as it takes to carry `scale` decimal places: `obase=16; 1/3` is
`.55554` at scale 6, `obase=2; 255` is `11111111`.

`ibase=16` reads `A`-`F` as digits (upper case only; lower case is always a
name). As in bc, `ibase=10` typed while in base 16 means sixteen; `ibase=A`
goes back to decimal.

### Errors

An error stops the rest of the line (statements before it have already
run). The line is shown again with a caret under the place and the column:
`divide by zero`, `syntax error`, `syntax error: missing )`,
`unknown function 'j'`, `square root of a negative number`,
`logarithm of zero or a negative number`, `exponent must be an integer`,
`overflow` (anything that would be infinite or not a number: `10^400`,
`e(1000)`), `scale must be 0 to 20`, `ibase must be 10 or 16`,
`obase must be 2, 8, 10 or 16`, `too many variables (32)`, `name too long`.
Nothing ever prints `inf` or `nan`.

## How it differs from GNU bc

- **Double precision, not arbitrary precision.** Numbers are IEEE doubles:
  about 15 significant digits. Digits beyond that are not invented - an
  answer is rounded to 15 significant digits before it is cut to `scale` -
  so `scale=20; 1/3` shows `.33333333333333300000`, and `2^100` shows
  `1267650600228230000000000000000` (bc: `...229401496703205376`). Integers
  print exactly up to 2^63. `scale` stops at 20.
- **`scale` starts at 6, not 0.** On a calculator `1/3` should not be `0`.
  Set `scale=0` for bc's integer behaviour.
- **`%` is the floating remainder** (`fmod`): `7 % 3` is 1 and `7.5 % 2` is
  1.5 at any scale. bc computes `a - (a/b)*b` at the current scale, which with
  scale 6 makes `7 % 3` come out as `.000001`.
- **No user functions, loops or if.** `define`, `if`, `while`, `for`,
  `return`, `break`, `auto` are refused with "not supported". So are arrays,
  strings, `print` and `read`, and the `-l` Bessel function `j(n,x)`.
- **`-l` functions are always there**, at the current scale; there is no
  `-l` switch.
- **Bases:** `ibase` is 10 or 16 and `obase` 2, 8, 10 or 16. Digits G-Z are
  not accepted.
- **Long numbers are not wrapped** with backslashes at 70 columns.
- `quit` takes effect when it is reached, not when the line is read, and
  only as a statement of its own (`q` is still usable as a variable name in
  `q=5`).
