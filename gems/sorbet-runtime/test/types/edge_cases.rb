# frozen_string_literal: true
require_relative '../test_helper'

class Opus::Types::Test::EdgeCasesTest < Critic::Unit::UnitTest
  it 'can type ==' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      sig {override.params(other: T.self_type).returns(T::Boolean)}
      def ==(other)
        true
      end
    end
    assert_equal(klass.new, klass.new)
  end

  it 'handles aliased methods' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      sig {returns(Symbol)}
      def foo
        :foo
      end
      alias_method :bar, :foo
    end
    assert_equal(:foo, klass.new.foo)
    assert_equal(:foo, klass.new.bar)
  end

  it 'works for any_instance' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      def foo
        raise "bad"
      end

      sig {returns(Symbol)}
      def bar
        raise "bad"
      end
    end

    klass.any_instance.stubs(:foo)
    klass.new.foo

    klass.any_instance.stubs(:bar).returns(:bar)
    klass.new.bar
  end

  it 'works for calls_original' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      sig {returns(Symbol)}
      def self.foo
        :foo
      end
    end

    # klass.stubs(:foo).calls_original # TODO not supported by Mocha
    assert_equal(:foo, klass.foo)
  end

  it 'works for stubbed superclasses with type' do
    parent = Class.new do
      extend T::Sig
      extend T::Helpers
      sig {overridable.returns(Symbol)}
      def self.foo
        :parent
      end
    end
    child = Class.new(parent) do
      extend T::Sig
      extend T::Helpers
      sig {override.returns(Symbol)}
      def self.foo
        :child
      end
    end
    parent.stubs(:foo)
    child.stubs(:foo).returns(:child_stub)
    assert_equal(:child_stub, child.foo)
  end

  it 'works for stubbed superclasses without type' do
    parent = Class.new do
      def self.foo
        :parent
      end
    end
    child = Class.new(parent) do
      extend T::Sig
      extend T::Helpers
      sig {override.returns(Symbol)}
      def self.foo
        :child
      end
    end
    parent.stubs(:foo)
    child.stubs(:foo).returns(:child_stub)
    assert_equal(:child_stub, child.foo)
  end

  it 'allows private abstract methods' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      abstract!

      sig {abstract.void}
      private def foo; end
    end
    T::Private::Abstract::Validate.validate_abstract_module(klass)
  end

  it 'handles class scope change when already hooked' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      sig {returns(Symbol)}
      def foo
        :foo
      end

      class << self
        extend T::Sig
        extend T::Helpers
        sig {returns(Symbol)}
        def foo
          :foo
        end
      end
    end
    assert_equal(:foo, klass.foo)
    assert_equal(:foo, klass.new.foo)
  end

  it 'handles class scope change when hooked from class << self' do
    klass = Class.new do
      class << self
        extend T::Sig

        sig {returns(Symbol)}
        def foo
          :foo
        end
      end

      extend T::Sig
      sig {returns(Symbol)}
      def bar
        :bar
      end
    end
    assert_equal(:foo, klass.foo)
    assert_equal(:bar, klass.new.bar)
  end

  it 'checks for type error in class << self' do
    klass = Class.new do
      class << self
        extend T::Sig

        sig {returns(Symbol)}
        def bad_return
          1
        end
      end
    end

    assert_raises(TypeError) do
      klass.bad_return
    end
  end

  it 'checks for type error for self.foo in class << self' do
    klass = Class.new do
      class << self
        extend T::Sig

        sig {returns(Symbol)}
        def self.bad_return
          1
        end

        def sanity; end
      end
    end

    klass.sanity
    assert_raises(TypeError) do
      klass.singleton_class.bad_return
    end
  end

  it "calls a user-defined singleton_method_added when registering hooks" do
    klass = Class.new do
      class << self
        def singleton_method_added(name)
          @called ||= []
          @called << name
        end
      end

      extend T::Sig
      sig {returns(Symbol)}
      def foo; end

      def self.post_hook; end
    end

    assert_equal(
      [
        :singleton_method_added,
        :post_hook,
      ],
      klass.instance_variable_get(:@called)
    )
  end

  it "allows custom hooks" do
    parent = Class.new do
      extend T::Sig
      sig {params(method: Symbol).void}
      def self.method_added(method)
        super(method)
      end
      def self.singleton_method_added(method)
        super(method)
      end
    end
    Class.new(parent) do
      extend T::Sig
      sig {void}
      def a; end
      sig {void}
      def b; end
      sig {void}
      def c; end
    end
  end

  it "allows well-behaved custom hooks" do
    c1 = Class.new do
      extend T::Sig

      sig {returns(Integer)}
      def foo; 1; end

      def self.method_added(name)
        super(name)
      end

      def self.singleton_method_added(name)
        super(name)
      end

      sig {returns(Integer)}
      def bar; "bad"; end
    end

    assert_equal(1, c1.new.foo)
    assert_raises(TypeError) { c1.new.bar }
  end

  it "does not make sig available to attached class" do
    assert_raises(NoMethodError) do
      Class.new do
        class << self
          extend T::Sig
          sig {void}
          def jojo; end
        end

        sig {void} # this shouldn't work since sig is not available
        def self.post; end
      end
    end
  end

  it 'keeps raising for bad sigs' do
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      sig {raise "foo"}
      def foo; end
    end
    instance = klass.new

    2.times do
      e = assert_raises {instance.foo}
      assert_equal("foo", e.message)
    end
  end

  it 'fails for sigs that fail then pass' do
    counter = 0
    klass = Class.new do
      extend T::Sig
      extend T::Helpers
      sig do
        counter += 1
        raise "foo" if counter == 1
        void
      end
      def foo; end
    end
    instance = klass.new

    e = assert_raises {instance.foo}
    assert_equal("foo", e.message)
    e = assert_raises {instance.foo}
    assert_match(/A previous invocation of #<UnboundMethod: /, e.message)
  end

end
