(module
  (memory 1)
  (func (export "_start") (result i32)
        i32.const 10
        i32.const 20
        drop ;; drop 20. then return 10
  )
)
