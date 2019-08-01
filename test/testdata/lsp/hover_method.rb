# typed: true
class Foo
  extend T::Sig

  sig {returns(Integer)}
  def bar
    # ^ hover: sig {returns(Integer)}
    # N.B. Checking two positions on below function call as they used to return different strings.
    baz("1")
  # ^ hover: sig {params(arg0: String).returns(Integer)}
   # ^ hover: sig {params(arg0: String).returns(Integer)}
  end

  sig {params(a: String).void}
  def self.bar(a)
         # ^ hover: ```ruby
         # ^ hover: sig {params(a: String).void}
         # ^ hover: ```
  end

  sig {params(arg0: String).returns(Integer)}
  def baz(arg0)
    no_args_and_void
  # ^ hover: sig {void}
    Foo::bat(1)
  # ^ hover: T.class_of(Foo)
       # ^ hover: sig {params(i: Integer).returns(Integer)}
           # ^ hover: Integer(1)
    arg0.to_i
  end

  sig {params(i: Integer).returns(Integer)}
  def self.bat(i)
    10
  end

  # Docs above single-line sig
  sig {params(arg0: Integer).void}
  def typed_with_docs(arg0)
    # ^ hover: Docs above single-line sig
    # ^ hover: sig {params(arg0: Integer).void}
  end

  sig {void}
  def qux
    typed_with_docs(1)
  # ^     ^       ^ hover: Docs above single-line sig
  # ^     ^       ^ hover: sig {params(arg0: Integer).void}
  end

  sig {void}
  def no_args_and_void
  end

  sig {returns(T.noreturn)}
  def always_raises
    # ^ sig {returns(T.noreturn)}
    raise RuntimeError
  end


  sig {params(blk: T.proc.params(arg0: Integer, arg1: String).returns(String)).returns(String)}
  def self.blk_arg(&blk)
    yield(1, "hello")
  # ^ hover: T.proc.params(arg0: Integer, arg1: String).returns(String)
  end

  sig {params(a: String, x: String).returns(T::Array[String])}
  def self.splat_arg(a, *x)
                       # ^ hover: String
    x << a
  # ^ hover: T::Array[String]
    x
  end
end

def main
  rv = Foo.blk_arg {|num, str| num.to_s + str}
# ^ hover: String
         # ^ hover: sig {params(blk: T.proc.params(arg0: Integer, arg1: String).returns(String)).returns(String)}
                   # ^ hover: sig {params(args: T.untyped).returns(Integer)}
                        # ^ hover: sig {params(args: T.untyped).returns(String)}
  rv2 = Foo.splat_arg("a", "b", "c")
# ^ hover: T::Array[String]
          # ^ hover: sig {params(a: String, x: String).returns(T::Array[String])}

end
