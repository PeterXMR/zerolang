## Status

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.math.minU32(left, right)` | `u32` | Returns the smaller unsigned value. |
| `std.math.maxU32(left, right)` | `u32` | Returns the larger unsigned value. |
| `std.math.clampU32(value, low, high)` | `u32` | Clamps between the two bounds; swapped bounds are normalized. |
| `std.math.gcdU32(left, right)` | `u32` | Euclidean greatest common divisor. |
| `std.math.lcmU32(left, right)` | `u32` | Least common multiple; returns `0` when either input is `0`. |
| `std.math.powU32(base, exponent)` | `u32` | Fixed-width exponentiation by squaring. |
| `std.math.modPowU32(base, exponent, modulus)` | `u32` | Modular exponentiation; returns `0` for modulus `0`. |
| `std.math.isPrimeU32(value)` | `Bool` | Trial division primality for unsigned integers. |
| `std.math.divisorCountU32(value)` | `u32` | Counts positive divisors; returns `0` for `0`. |
| `std.math.properDivisorSumU32(value)` | `u32` | Sums positive divisors smaller than `value`. |

Current scope:

- Helpers are pure, target-neutral fixed-width integer operations.
- Arithmetic follows Zero's current fixed-width integer behavior.
- The module does not provide floating-point math, big integers, or arbitrary-precision number theory.

## Example

```zero
pub fn main(world: World) -> Void raises {
    if std.math.gcdU32(84, 30) == 6 && std.math.isPrimeU32(31) {
        check world.out.write("math helper ok\n")
    }
}
```

## Design Notes

`std.math` does not allocate and does not require a hosted runtime capability.
Names include the integer width because Zero does not overload standard-library
helpers by argument type.

Number-theory helpers are intentionally simple and deterministic. They are
suitable for small fixed-width tasks, examples, and compiler-portable checks.
Large-number algorithms should be added as explicit APIs with documented bounds
instead of hidden heap allocation or implicit widening.
