(module
  (memory 1)
  (data (i32.const 0) "abcdefghijklmnopqrstuvwxyz")

  (func (export "_start") (result i64)
        (i64.load16_s offset=0 (i32.const 0)) ;; 25185 'ab'
  )
)
