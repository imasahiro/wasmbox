(module
  (memory 1)
  (func (export "_start") (param i32 i32) (result i32)
        (i32.rotr (local.get 0) (local.get 1))
  )
)
