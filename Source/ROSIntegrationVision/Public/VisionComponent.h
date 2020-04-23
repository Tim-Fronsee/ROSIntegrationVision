// Author: Tim Fronsee <tfronsee21@gmail.com>
#pragma once

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"

#include "VisionFormat.h"
#include "RI/Topic.h"

#include "VisionComponent.generated.h"

UCLASS()
class ROSINTEGRATIONVISION_API UVisionComponent : public UCameraComponent
{
  GENERATED_BODY()

public:
  UVisionComponent();
  ~UVisionComponent();

  void SetFramerate(const float _FrameRate);
  void Pause(const bool _Pause = true);
  bool IsPaused() const;

  // Used with the color capture to apply a contrast adjustment.
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    TEnumAsByte<EVisionFormat> Format;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    FString ParentLink; // Defines the link that binds to the image frame.
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    bool DisableTFPublishing;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    uint32 Width;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    uint32 Height;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    float Framerate;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    bool UseEngineFramerate;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    int32 ServerPort;
  // Used with the color capture to apply a gamma adjustment.
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    float GammaCorrection;
  // Used with the color capture to apply a brightness adjustment.
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    float Brightness;
  // Used with the color capture to apply a contrast adjustment.
  UPROPERTY(EditAnywhere, Category = "Vision Component")
    float Contrast;

  // The cameras for color, depth and objects;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
	  USceneCaptureComponent2D * Color;
  UPROPERTY(EditAnywhere, Category = "Vision Component")
  	USceneCaptureComponent2D * Depth;

	UPROPERTY(EditAnywhere, Category = "Vision Component")
		UTopic * CameraInfoPublisher;
	UPROPERTY(EditAnywhere, Category = "Vision Component")
		UTopic * DepthPublisher;
	UPROPERTY(EditAnywhere, Category = "Vision Component")
		UTopic * ImagePublisher;
	UPROPERTY(EditAnywhere, Category = "Vision Component")
		UTopic * TFPublisher;

  UPROPERTY(BlueprintReadWrite, Category = "Vision Component")
    FString ImageFrame = TEXT("/unreal_ros/image_frame");
  UPROPERTY(BlueprintReadWrite, Category = "Vision Component")
    FString ImageOpticalFrame = TEXT("/unreal_ros/image_optical_frame");

protected:

  virtual void InitializeComponent() override;
  virtual void BeginPlay() override;
  virtual void TickComponent(float DeltaTime,
                             enum ELevelTick TickType,
                             FActorComponentTickFunction *TickFunction) override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  float FrameTime, TimePassed;

private:

	// Private data container
	class PrivateData;
	PrivateData *Priv;

	UMaterialInstanceDynamic *MaterialDepthInstance;

  TArray<FColor> ImageColor;
  TArray<FLinearColor> ImageLinearColor;
  TArray<FFloat16Color> ImageFloat16Color, ImageDepth;
  TArray<uint8> DataColor, DataDepth;
  TArray<FColor> ObjectColors;
  TMap<FString, uint32> ObjectToColor;
  uint32 ColorsUsed;
  bool Running, Paused;

  void ReadColor(UTextureRenderTarget2D *RenderTarget, TArray<FColor> &ImageData) const;
  void ColorToBytes(const TArray<FColor> &ImageData, uint8 *Bytes) const;
  void ReadLinearColor(UTextureRenderTarget2D *RenderTarget, TArray<FLinearColor> &ImageData) const;
  void LinearColorToBytes(const TArray<FLinearColor> &ImageData, uint8 *Bytes) const;
  void ReadFloat16Color(UTextureRenderTarget2D *RenderTarget, TArray<FFloat16Color> &ImageData) const;
  void Float16ColorToBytes(const TArray<FFloat16Color> &ImageData, uint8 *Bytes) const;
  void ReadImageCompressed(UTextureRenderTarget2D *RenderTarget, TArray<FFloat16Color> &ImageData) const;
  void ToDepthImage(const TArray<FFloat16Color> &ImageData, uint8 *Bytes) const;
  void StoreImage(const uint8 *ImageData, const uint32 Size, const char *Name) const;
  void GenerateColors(const uint32_t NumberOfColors);
  bool ColorObject(AActor *Actor, const FString &name);
  bool ColorAllObjects();
  void ProcessColor();
  void ProcessDepth();

  // Applies image processing techniques to a channel.
  uint8_t ProcessChannel(const uint8 &channel) const;
  uint8_t ProcessChannel(const float &channel) const;
  uint8_t ProcessChannel(const FFloat16 &channel) const;

  // in must hold Width*Height*2(float) Bytes
  void convertDepth(const uint16_t *in, __m128 *out) const;
};
