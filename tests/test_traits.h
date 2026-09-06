#pragma once

#include <slick/queue.h>

// Feature configuration is a template parameter, so one test binary can cover every
// combination. Each traits type below pins the features a group of tests needs, instead
// of relying on a build-wide -D that would leave the shipped default untested.

/// Loss detection on, everything else at its default.
struct loss_traits : slick::queue_traits {
  static constexpr bool enable_loss_detection = true;
};

/// The opt-in reset-detection path, plus loss detection so reset() can clear it.
struct reset_check_traits : slick::queue_traits {
  static constexpr bool enable_reset_check = true;
  static constexpr bool enable_loss_detection = true;
};

/// read_last() disabled - the lean producer configuration.
struct no_read_last_traits : slick::queue_traits {
  static constexpr bool enable_read_last = false;
};
