(module
  (memory 2)
  (data (i32.const 0) "abcdefghijklmnopqrstuvwxyz")

  (func (export "_start") (result i32)
        (i32.load8_u offset=0 (i32.const 0)) ;; 97 'a'
  )
)
