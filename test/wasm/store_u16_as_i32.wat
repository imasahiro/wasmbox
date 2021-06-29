(module
  (memory 1)
  (data (i32.const 0) "ABCDEFGHIJKLMNOPQRSTUVWXYZ")

  (func (export "_start") (param i32) (result i32)
        (i32.store16 offset=0 (i32.const 0) (local.get 0))
        (i32.load16_u offset=0 (i32.const 0))  ;; 25185 'ab'
  )
)
