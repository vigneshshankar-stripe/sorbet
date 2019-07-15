# typed: true

module Opus; end

module Opus::Mixin; end
module Opus
  MixinAlias = Opus::Mixin
end

class Opus::Mixed
  include Opus::MixinAlias
end

module Opus::MixedModule
  include Opus::MixinAlias
end

class Opus::MixedDescendant # Mixes in Opus::Mixed via Opus::MixedModule
  include Opus::MixedModule
end