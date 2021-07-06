(module
  (func $main (export "_start") (result i32) (local $i i32)
    ;; i32 x = 0
    i32.const 0
    set_local $i
    block $outer
      ;; L_inner;
      loop $inner
        ;; i += 1
        get_local $i
        i32.const 1
        i32.add
        set_local $i
        ;; cond = i == 10
        get_local $i
        i32.const 10
        i32.eq
        ;; if(cond) { goto L_outer; }
        br_if $outer
        ;; goto L_inner;
        br $inner
      end
    end
    ;; L_outer
    get_local $i ;; == 10
  )
)
