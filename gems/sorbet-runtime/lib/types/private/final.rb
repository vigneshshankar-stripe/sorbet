# frozen_string_literal: true
# typed: false

module T::Private::Final
  module NoInherit
    def inherited(arg)
      super(arg)
      raise "#{self.name} was declared as final and cannot be inherited"
    end
  end

  module NoIncludeExtend
    def included(arg)
      super(arg)
      raise "#{self.name} was declared as final and cannot be included"
    end

    def extended(arg)
      super(arg)
      raise "#{self.name} was declared as final and cannot be extended"
    end
  end

  def self.declare(mod)
    if !mod.is_a?(Module)
      raise "#{mod.inspect} is not a class or module and cannot be declared as final with `final!`"
    end
    if final_module?(mod)
      raise "#{mod.name} was already declared as final and cannot be re-declared as final"
    end
    if T::AbstractUtils.abstract_module?(mod)
      raise "#{mod.name} was already declared as abstract and cannot be declared as final"
    end
    mod.extend(mod.is_a?(Class) ? NoInherit : NoIncludeExtend)
    mark_as_final_module(mod)
    mark_as_final_module(mod.singleton_class)
    T::Private::Methods.add_module_with_final(mod)
    T::Private::Methods.install_hooks(mod)
  end

  def self.final_module?(mod)
    mod.instance_variable_defined?(:@sorbet_final_module)
  end

  private_class_method def self.mark_as_final_module(mod)
    mod.instance_variable_set(:@sorbet_final_module, true)
  end
end
