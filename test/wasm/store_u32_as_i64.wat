(module
  (memory 1)
  (data (i32.const 0) "ABCDEFGHIJKLMNOPQRSTUVWXYZ")

  (func (export "_start") (param i64) (result i64)
        (i64.store32 offset=0 (i32.const 0) (local.get 0))
        (i64.load32_u offset=0 (i32.const 0))  ;; 1684234849 'abcd'
  )
)
