import data.num
using num

variable f : num → num → num → num

check
  let a := 10
  in f a 10

/-
check
  let a := 10,
      b := 10
  in f a b 10
-/
