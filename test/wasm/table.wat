(module
  (type $sig (func (param i32 i32 i32) (result i32)))
  (table funcref (elem $f))
  ;; function f(a, b, c,) { return b; }
  (func $f (param i32 i32 i32) (result i32) (local.get 1))

  (func (export "_start") (result i32) ;; == 2
        (call_indirect (type $sig) (i32.const 0) 
        (i32.const 1) (i32.const 2) (i32.const 3)
    )
  )
)
