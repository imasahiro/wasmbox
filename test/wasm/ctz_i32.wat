(module
  (memory 1)
  (func (export "_start") (param i32) (result i32)
        (i32.ctz (local.get 0))
  )
)
