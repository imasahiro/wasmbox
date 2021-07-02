(module
  (memory 1)
  (func (export "_start") (param i64) (result i64)
        (i64.clz (local.get 0))
  )
)
