(module
  (memory 1)
  (data (i32.const 0) "ABCDEFGHIJKLMNOPQRSTUVWXYZ")

  (func (export "_start") (param i64) (result i64)
        (i64.store offset=0 (i32.const 0) (local.get 0))
        (i64.load offset=0 (i32.const 0))  ;; 7523094288207667809 'abcdefgh'
  )
)
