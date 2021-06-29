(module
  (memory 1)
  (data (i32.const 0) "abcdefghijklmnopqrstuvwxyz")

  (func (export "_start") (result i32)
        (i32.load8_s offset=0 (i32.const 0)) ;; 97 'a'
  )
)
