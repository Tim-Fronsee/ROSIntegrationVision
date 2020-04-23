// Author: Tim Fronsee <tfronsee21@gmail.com>
#pragma once

#include "VisionFormat.generated.h"

UENUM()
enum EVisionFormat
{
  Color        UMETA(DisplayName = "Color"),
  LinearColor  UMETA(DisplayName = "Linear Color"),
  Float16Color UMETA(DisplayName = "Float16 Color")
};
