# frozen_string_literal: true
# typed: true

module T::Private::Methods
  Declaration = Struct.new(:mod, :params, :returns, :bind, :mode, :checked, :finalized, :on_failure, :override_allow_incompatible, :type_parameters, :generated)

  class DeclBuilder
    attr_reader :decl

    class BuilderError < StandardError; end

    private def check_live!
      if decl.finalized
        raise BuilderError.new("You can't modify a signature declaration after it has been used.")
      end
    end

    def initialize(mod)
      # TODO RUBYPLAT-1278 - with ruby 2.5, use kwargs here
      @decl = Declaration.new(
        mod,
        ARG_NOT_PROVIDED, # params
        ARG_NOT_PROVIDED, # returns
        ARG_NOT_PROVIDED, # bind
        Modes.standard, # mode
        ARG_NOT_PROVIDED, # checked
        false, # finalized
        ARG_NOT_PROVIDED, # on_failure
        nil, # override_allow_incompatible
        ARG_NOT_PROVIDED, # type_parameters
        ARG_NOT_PROVIDED, # generated
      )
    end

    def params(params)
      check_live!
      if !decl.params.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .params twice")
      end

      decl.params = params

      self
    end

    def returns(type)
      check_live!
      if decl.returns.is_a?(T::Private::Types::Void)
        raise BuilderError.new("You can't call .returns after calling .void.")
      end
      if !decl.returns.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .returns multiple times in a signature.")
      end

      decl.returns = type

      self
    end

    def void
      check_live!
      if !decl.returns.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .void after calling .returns.")
      end

      decl.returns = T::Private::Types::Void.new

      self
    end

    def bind(type)
      check_live!
      if !decl.bind.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .bind multiple times in a signature.")
      end

      decl.bind = type

      self
    end

    def checked(level)
      check_live!

      if !decl.checked.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .checked multiple times in a signature.")
      end
      if level == :never && !decl.on_failure.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't use .checked(:never) with .on_failure because .on_failure will have no effect.")
      end
      if !decl.generated.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't use .checked with .generated.")
      end
      if !T::Private::RuntimeLevels::LEVELS.include?(level)
        raise BuilderError.new("Invalid `checked` level '#{level}'. Use one of: #{T::Private::RuntimeLevels::LEVELS}.")
      end

      decl.checked = level

      self
    end

    def on_failure(*args)
      check_live!

      if !decl.on_failure.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .on_failure multiple times in a signature.")
      end
      if decl.checked == :never
        raise BuilderError.new("You can't use .on_failure with .checked(:never) because .on_failure will have no effect.")
      end
      if !decl.generated.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't use .on_failure with .generated.")
      end

      decl.on_failure = args

      self
    end

    def generated
      check_live!

      if !decl.generated.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .generated multiple times in a signature.")
      end
      if !decl.checked.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't use .generated with .checked.")
      end
      if !decl.on_failure.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't use .generated with .on_failure.")
      end

      decl.generated = true

      self
    end

    def abstract
      check_live!

      case decl.mode
      when Modes.standard
        decl.mode = Modes.abstract
      when Modes.abstract
        raise BuilderError.new(".abstract cannot be repeated in a single signature")
      else
        raise BuilderError.new("`.abstract` cannot be combined with any of `.override`, `.implementation`, or "\
              "`.overridable`.")
      end

      self
    end

    def final
      check_live!
      raise BuilderError.new("The syntax for declaring a method final is `sig(:final) {...}`, not `sig {final. ...}`")
    end

    def override(allow_incompatible: false)
      check_live!

      case decl.mode
      when Modes.standard
        decl.mode = Modes.override
        decl.override_allow_incompatible = allow_incompatible
      when Modes.override
        raise BuilderError.new(".override cannot be repeated in a single signature")
      else
        raise BuilderError.new("`.override` cannot be combined with any of `.abstract`, `.implementation`, or "\
              "`.overridable`.")
      end

      self
    end

    def overridable
      check_live!

      case decl.mode
      when Modes.abstract, Modes.override
        raise BuilderError.new("`.overridable` cannot be combined with `.#{decl.mode}`")
      when Modes.standard
        decl.mode = Modes.overridable
      when Modes.implementation
        decl.mode = Modes.overridable_implementation
      when Modes.overridable, Modes.overridable_implementation
        raise BuilderError.new(".overridable cannot be repeated in a single signature")
      end

      self
    end

    def implementation
      check_live!

      case decl.mode
      when Modes.abstract, Modes.override
        raise BuilderError.new("`.implementation` cannot be combined with `.#{decl.mode}`")
      when Modes.standard
        decl.mode = Modes.implementation
      when Modes.overridable
        decl.mode = Modes.overridable_implementation
      when Modes.implementation, Modes.overridable_implementation
        raise BuilderError.new(".implementation cannot be repeated in a single signature")
      end

      self
    end

    # Declares valid type paramaters which can be used with `T.type_parameter` in
    # this `sig`.
    #
    # This is used for generic methods. Example usage:
    #
    #  sig do
    #    type_parameters(:U)
    #    .params(blk: T.proc.params(arg0: Elem).returns(T.type_parameter(:U)))
    #    .returns(T::Array[T.type_parameter(:U)])
    #  end
    #  def map(&blk); end
    def type_parameters(*names)
      check_live!

      names.each do |name|
        raise BuilderError.new("not a symbol: #{name}") unless name.is_a?(Symbol)
      end

      if !decl.type_parameters.equal?(ARG_NOT_PROVIDED)
        raise BuilderError.new("You can't call .type_parameters multiple times in a signature.")
      end

      decl.type_parameters = names

      self
    end

    def finalize!
      check_live!

      if decl.bind.equal?(ARG_NOT_PROVIDED)
        decl.bind = nil
      end
      if decl.checked.equal?(ARG_NOT_PROVIDED)
        default_checked_level = T::Private::RuntimeLevels.default_checked_level
        if default_checked_level == :never && !decl.on_failure.equal?(ARG_NOT_PROVIDED)
          raise BuilderError.new("To use .on_failure you must additionally call .checked(:tests) or .checked(:always), otherwise, the .on_failure has no effect.")
        end
        decl.checked = default_checked_level
      end
      if decl.on_failure.equal?(ARG_NOT_PROVIDED)
        decl.on_failure = nil
      end
      if decl.generated.equal?(ARG_NOT_PROVIDED)
        decl.generated = false
      end
      if decl.params.equal?(ARG_NOT_PROVIDED)
        decl.params = {}
      end
      if decl.type_parameters.equal?(ARG_NOT_PROVIDED)
        decl.type_parameters = {}
      end

      decl.finalized = true

      self
    end
  end
end
