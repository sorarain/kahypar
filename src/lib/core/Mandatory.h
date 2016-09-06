/***************************************************************************
 *  Copyright (C) 2015 Sebastian Schlag <sebastian.schlag@kit.edu>
 **************************************************************************/

#pragma once

// http://clean-cpp.org/mandatory-template-arguments/

#include <type_traits>

namespace core {
struct unspecified_type;

template <typename SomeType>
class MandatoryTemplateArgument {
  struct private_type;

  static_assert(std::is_same<SomeType, private_type>::value,
                "You forgot to specify a mandatory template argument which cannot be deduced."
                );
};
}  // namespace core

using Mandatory = core::MandatoryTemplateArgument<core::unspecified_type>;

template <typename T>
using MandatoryTemplate = core::MandatoryTemplateArgument<T>;
