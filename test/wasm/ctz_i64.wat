(module
  (memory 1)
  (func (export "_start") (param i64) (result i64)
        (i64.ctz (local.get 0))
  )
)
