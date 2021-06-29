(module
  (memory 1)
  (data (i32.const 0) "abcdefghijklmnopqrstuvwxyz")

  (func (export "_start") (result i64)
        (i64.load32_u offset=0 (i32.const 0)) ;; 1684234849 'abcd'
  )
)
