(module
  (func (export "_start") (param i32 i32 i32) (result i32)
        (if (result i32) (i32.eqz (local.get 0)) (then (local.get 1)) (else (local.get 2)))
  )
)
