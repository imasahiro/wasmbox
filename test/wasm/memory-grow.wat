(module
  (memory 2)
  (func (export "_start") (param i32) (result i32)
        (memory.grow (local.get 0))
  )
)
