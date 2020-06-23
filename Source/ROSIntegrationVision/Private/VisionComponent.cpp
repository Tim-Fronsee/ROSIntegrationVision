// Author Tim Fronsee <tfronsee21@gmail.com>
#include "VisionComponent.h"

#include <cmath>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include "immintrin.h"

#include "ROSTime.h"
#include "sensor_msgs/CameraInfo.h"
#include "sensor_msgs/Image.h"
#include "tf2_msgs/TFMessage.h"

#include "EngineUtils.h"
#include "GenericPlatform/GenericPlatformMath.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "UObject/ConstructorHelpers.h"

#if PLATFORM_WINDOWS
  #define _USE_MATH_DEFINES
#endif

// Private data container so that internal structures are not visible to the outside
class ROSINTEGRATIONVISION_API UVisionComponent::PrivateData
{
public:
	TSharedPtr<PacketBuffer> Buffer;
	// TCPServer Server;
	std::mutex WaitColor, WaitDepth, WaitDone;
	std::condition_variable CVColor, CVDepth;
	std::thread ThreadColor, ThreadDepth;
};

UVisionComponent::UVisionComponent() :
Format(EVisionFormat::Color),
Width(960),
Height(540),
Framerate(1),
UseEngineFramerate(false),
ServerPort(10000),
GammaCorrection(1.0f),
Brightness(0.f),
Contrast(1.f),
FrameTime(1.0f / Framerate),
TimePassed(0),
ColorsUsed(0)
{
    Priv = new PrivateData();
    FieldOfView = 90.0;
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;

    auto owner = GetOwner();
    if (owner)
    {
        Color = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ColorCapture"));
        Color->SetupAttachment(this);
        Color->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        Color->TextureTarget = CreateDefaultSubobject<UTextureRenderTarget2D>(TEXT("ColorTarget"));
        Color->TextureTarget->InitAutoFormat(Width, Height);
        Color->FOVAngle = FieldOfView;

        Depth = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("DepthCapture"));
        Depth->SetupAttachment(this);
        Depth->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
        Depth->TextureTarget = CreateDefaultSubobject<UTextureRenderTarget2D>(TEXT("DepthTarget"));
        Depth->TextureTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
        Depth->TextureTarget->InitAutoFormat(Width, Height);
        Depth->FOVAngle = FieldOfView;
    }
    else {
        UE_LOG(LogTemp, Warning, TEXT("No owner!"));
    }

    CameraInfoPublisher = NewObject<UTopic>(UTopic::StaticClass());
    DepthPublisher = NewObject<UTopic>(UTopic::StaticClass());
    ImagePublisher = NewObject<UTopic>(UTopic::StaticClass());
    TFPublisher = NewObject<UTopic>(UTopic::StaticClass());
}

UVisionComponent::~UVisionComponent()
{
    delete Priv;
}

void UVisionComponent::SetFramerate(const float _Framerate)
{
    Framerate = _Framerate;
    FrameTime = 1.0f / _Framerate;
    TimePassed = 0;
}

void UVisionComponent::Pause(const bool _Pause)
{
    Paused = _Pause;
}

bool UVisionComponent::IsPaused() const
{
    return Paused;
}

void UVisionComponent::InitializeComponent()
{
    Super::InitializeComponent();
}

void UVisionComponent::BeginPlay()
{
  Super::BeginPlay();
    // Initializing buffers for reading images from the GPU
	ImageColor.AddUninitialized(Width * Height);
  ImageLinearColor.AddUninitialized(Width * Height);
  ImageFloat16Color.AddUninitialized(Width * Height);
	ImageDepth.AddUninitialized(Width * Height);

	// Reinit renderer
	Color->TextureTarget->InitAutoFormat(Width, Height);
	Depth->TextureTarget->InitAutoFormat(Width, Height);

	AspectRatio = Width / (float)Height;

	// Creating double buffer and setting the pointer of the server object
	Priv->Buffer = TSharedPtr<PacketBuffer>(new PacketBuffer(Width, Height, FieldOfView));

	Running = true;
	Paused = false;

	// Starting threads to process image data
	Priv->ThreadColor = std::thread(&UVisionComponent::ProcessColor, this);
	Priv->ThreadDepth = std::thread(&UVisionComponent::ProcessDepth, this);

	// Establish ROS communication
	UROSIntegrationGameInstance* rosinst = Cast<UROSIntegrationGameInstance>(GetOwner()->GetGameInstance());
	if (rosinst)
	{
		TFPublisher->Init(rosinst->ROSIntegrationCore,
                      TEXT("/tf"),
                      TEXT("tf2_msgs/TFMessage"));

		CameraInfoPublisher->Init(rosinst->ROSIntegrationCore,
                              TEXT("/unreal_ros/camera_info"),
                              TEXT("sensor_msgs/CameraInfo"));
		CameraInfoPublisher->Advertise();

		ImagePublisher->Init(rosinst->ROSIntegrationCore,
                         TEXT("/unreal_ros/image_color"),
                         TEXT("sensor_msgs/Image"));
		ImagePublisher->Advertise();

		DepthPublisher->Init(rosinst->ROSIntegrationCore,
                         TEXT("/unreal_ros/image_depth"),
                         TEXT("sensor_msgs/Image"));
		DepthPublisher->Advertise();
	}
	else {
		UE_LOG(LogTemp, Warning, TEXT("UnrealROSInstance not existing."));
	}
	SetFramerate(Framerate); // Update framerate
}

void UVisionComponent::TickComponent(float DeltaTime,
                                     enum ELevelTick TickType,
                                     FActorComponentTickFunction *TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    // Check if paused
	if (Paused)
	{
		return;
	}

	// Check for framerate
	TimePassed += DeltaTime;
	if (!UseEngineFramerate && TimePassed < FrameTime)
	{
		return;
	}
	TimePassed -= FrameTime;
	MEASURE_TIME("Tick");

	auto owner = GetOwner();
	owner->UpdateComponentTransforms();

	FDateTime Now = FDateTime::UtcNow();
	Priv->Buffer->HeaderWrite->TimestampCapture = Now.ToUnixTimestamp() * 1000000000 + Now.GetMillisecond() * 1000000;

	FVector Translation = GetRelativeLocation();
	FQuat Quat = FQuat(GetRelativeRotation());

	// Convert to meters and ROS coordinate system in relation to the owner's transform.
	Priv->Buffer->HeaderWrite->Translation.X = Translation.X / 100.0f;
	Priv->Buffer->HeaderWrite->Translation.Y = -Translation.Y / 100.0f;
	Priv->Buffer->HeaderWrite->Translation.Z = Translation.Z / 100.0f;
	Priv->Buffer->HeaderWrite->Rotation.X = -Quat.X;
	Priv->Buffer->HeaderWrite->Rotation.Y = Quat.Y;
	Priv->Buffer->HeaderWrite->Rotation.Z = -Quat.Z;
	Priv->Buffer->HeaderWrite->Rotation.W = Quat.W;

	// Start writing to buffer
	Priv->Buffer->StartWriting(ObjectToColor, ObjectColors);

	// Read color image and notify processing thread
	Priv->WaitColor.lock();

  // Read the image data based on the desired format.
  switch (Format)
  {
    case EVisionFormat::LinearColor:
  	  ReadLinearColor(Color->TextureTarget, ImageLinearColor);
      break;
    case EVisionFormat::Float16Color:
      ReadFloat16Color(Color->TextureTarget, ImageFloat16Color);
      break;
    default:
      ReadColor(Color->TextureTarget, ImageColor);
      break;
  }

	Priv->WaitColor.unlock();
	Priv->CVColor.notify_one();

  // Read depth image.
  Priv->WaitDepth.lock();
  ReadFloat16Color(Depth->TextureTarget, ImageDepth);
  Priv->WaitDepth.unlock();
  Priv->CVDepth.notify_one();

  // Close the buffer.
  Priv->Buffer->DoneWriting();

	Priv->Buffer->StartReading();
	uint32_t xSize = Priv->Buffer->HeaderRead->Size;
	uint32_t xSizeHeader = Priv->Buffer->HeaderRead->SizeHeader; // Size of the header
	uint32_t xMapEntries = Priv->Buffer->HeaderRead->MapEntries; // Number of map entries at the end of the packet
	uint32_t xWidth = Priv->Buffer->HeaderRead->Width; // Width of the images
	uint32_t xHeight = Priv->Buffer->HeaderRead->Height; // Height of the images

	// Get the data offsets for the different types of images that are in the buffer
	const uint32_t& OffsetColor = Priv->Buffer->OffsetColor;
	const uint32_t& OffsetDepth = Priv->Buffer->OffsetDepth;
	const uint32_t& OffsetObject = Priv->Buffer->OffsetObject;
	// * - Depth image data (width * height * 2 Bytes (Float16))
	uint8_t* DepthPtr = &Priv->Buffer->Read[OffsetDepth];
	uint32_t TargetDepthBufSize = Width*Height * 4;
	uint8_t* TargetDepthBuf = new uint8_t[TargetDepthBufSize]; // Allocate a byte for every pixel * 4 Bytes for a single 32Bit Float

	const uint32_t ColorImageSize = Width * Height * 3;
	convertDepth((uint16_t *)DepthPtr, (__m128*)TargetDepthBuf);

	UE_LOG(LogTemp, Verbose, TEXT("Buffer Offsets: %d %d %d"), OffsetColor, OffsetDepth, OffsetObject);

	FROSTime time = FROSTime::Now();

  if (ImagePublisher) {
  	TSharedPtr<ROSMessages::sensor_msgs::Image> ImageMessage(new ROSMessages::sensor_msgs::Image());

  	ImageMessage->header.seq = 0;
  	ImageMessage->header.time = time;
  	ImageMessage->header.frame_id = ImageOpticalFrame;
  	ImageMessage->height = Height;
  	ImageMessage->width = Width;
  	ImageMessage->encoding = TEXT("bgr8");
  	ImageMessage->step = Width * 3;
  	ImageMessage->data = &Priv->Buffer->Read[OffsetColor];
  	ImagePublisher->Publish(ImageMessage);
  }

  if (DepthPublisher)
  {
  	TSharedPtr<ROSMessages::sensor_msgs::Image> DepthMessage(new ROSMessages::sensor_msgs::Image());

  	DepthMessage->header.seq = 0;
  	DepthMessage->header.time = time;
  	DepthMessage->header.frame_id = ImageOpticalFrame;
  	DepthMessage->height = Height;
  	DepthMessage->width = Width;
  	DepthMessage->encoding = TEXT("32FC1");
  	DepthMessage->step = Width * 4;
  	DepthMessage->data = TargetDepthBuf;
  	DepthPublisher->Publish(DepthMessage);
  }

	Priv->Buffer->DoneReading();

	if (!DisableTFPublishing) {
    // Start advertising TF only if it has yet to advertise.
    if (TFPublisher && !TFPublisher->IsAdvertising())
    {
      TFPublisher->Advertise();
    }
		TSharedPtr<ROSMessages::tf2_msgs::TFMessage> TFImageFrame(new ROSMessages::tf2_msgs::TFMessage());
		ROSMessages::geometry_msgs::TransformStamped TransformImage;
		TransformImage.header.seq = 0;
		TransformImage.header.time = time;
		TransformImage.header.frame_id = ParentLink;
		TransformImage.child_frame_id = ImageFrame;
		TransformImage.transform.translation.x = Priv->Buffer->HeaderRead->Translation.X;
		TransformImage.transform.translation.y = Priv->Buffer->HeaderRead->Translation.Y;
		TransformImage.transform.translation.z = Priv->Buffer->HeaderRead->Translation.Z;
		TransformImage.transform.rotation.x = Priv->Buffer->HeaderRead->Rotation.X;
		TransformImage.transform.rotation.y = Priv->Buffer->HeaderRead->Rotation.Y;
		TransformImage.transform.rotation.z = Priv->Buffer->HeaderRead->Rotation.Z;
		TransformImage.transform.rotation.w = Priv->Buffer->HeaderRead->Rotation.W;

		TFImageFrame->transforms.Add(TransformImage);

		TFPublisher->Publish(TFImageFrame);

		// Publish optical frame with a fixed joint connecting to the Image frame.
		FRotator OpticalRotator(0.0, -90.0, 90.0);
		FQuat OpticalQuat(OpticalRotator);

		TSharedPtr<ROSMessages::tf2_msgs::TFMessage> TFOpticalFrame(new ROSMessages::tf2_msgs::TFMessage());
		ROSMessages::geometry_msgs::TransformStamped TransformOptical;
		TransformOptical.header.seq = 0;
		TransformOptical.header.time = time;
		TransformOptical.header.frame_id = ImageFrame;
		TransformOptical.child_frame_id = ImageOpticalFrame;
		TransformOptical.transform.rotation.x = OpticalQuat.X;
		TransformOptical.transform.rotation.y = OpticalQuat.Y;
		TransformOptical.transform.rotation.z = OpticalQuat.Z;
		TransformOptical.transform.rotation.w = OpticalQuat.W;

		TFOpticalFrame->transforms.Add(TransformOptical);

		TFPublisher->Publish(TFOpticalFrame);
	}
  // Stop advertising if TF has been disabled and is already advertising.
  else if (TFPublisher && TFPublisher->IsAdvertising()) {
    TFPublisher->Unadvertise();
  }

	// Construct and publish CameraInfo

	const float FOVX = Height > Width ? FieldOfView * Width / Height : FieldOfView;
	const float FOVY = Width > Height ? FieldOfView * Height / Width : FieldOfView;
	double halfFOVX = FOVX * PI / 360.0; // was M_PI on gcc
	double halfFOVY = FOVY * PI / 360.0; // was M_PI on gcc
	const double cX = Width / 2.0;
	const double cY = Height / 2.0;

	const double K0 = cX / std::tan(halfFOVX);
	const double K2 = cX;
	const double K4 = K0;
	const double K5 = cY;
	const double K8 = 1;

	const double P0 = K0;
	const double P2 = K2;
	const double P5 = K4;
	const double P6 = K5;
	const double P10 = 1;

	TSharedPtr<ROSMessages::sensor_msgs::CameraInfo> CamInfo(new ROSMessages::sensor_msgs::CameraInfo());
	CamInfo->header.seq = 0;
	CamInfo->header.time = time;
	//CamInfo->header.frame_id =
	CamInfo->height = Height;
	CamInfo->width = Width;
	CamInfo->distortion_model = TEXT("plumb_bob");
	CamInfo->D[0] = 0;
	CamInfo->D[1] = 0;
	CamInfo->D[2] = 0;
	CamInfo->D[3] = 0;
	CamInfo->D[4] = 0;

	CamInfo->K[0] = K0;
	CamInfo->K[1] = 0;
	CamInfo->K[2] = K2;
	CamInfo->K[3] = 0;
	CamInfo->K[4] = K4;
	CamInfo->K[5] = K5;
	CamInfo->K[6] = 0;
	CamInfo->K[7] = 0;
	CamInfo->K[8] = K8;

	CamInfo->R[0] = 1;
	CamInfo->R[1] = 0;
	CamInfo->R[2] = 0;
	CamInfo->R[3] = 0;
	CamInfo->R[4] = 1;
	CamInfo->R[5] = 0;
	CamInfo->R[6] = 0;
	CamInfo->R[7] = 0;
	CamInfo->R[8] = 1;

	CamInfo->P[0] = P0;
	CamInfo->P[1] = 0;
	CamInfo->P[2] = P2;
	CamInfo->P[3] = 0;
	CamInfo->P[4] = 0;
	CamInfo->P[5] = P5;
	CamInfo->P[6] = P6;
	CamInfo->P[7] = 0;
	CamInfo->P[8] = 0;
	CamInfo->P[9] = 0;
	CamInfo->P[10] = P10;
	CamInfo->P[11] = 0;

	CamInfo->binning_x = 0;
	CamInfo->binning_y = 0;

	CamInfo->roi.x_offset = 0;
	CamInfo->roi.y_offset = 0;
	CamInfo->roi.height = 0;
	CamInfo->roi.width = 0;
	CamInfo->roi.do_rectify = false;

	CameraInfoPublisher->Publish(CamInfo);

	// Clean up
	delete[] TargetDepthBuf;
}

void UVisionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
    Running = false;

    // Stopping processing threads
    Priv->CVColor.notify_one();
    Priv->CVDepth.notify_one();

    Priv->ThreadColor.join();
    Priv->ThreadDepth.join();
}

void UVisionComponent::ReadColor(UTextureRenderTarget2D *RenderTarget, TArray<FColor> &ImageData) const
{
	FTextureRenderTargetResource *RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	RenderTargetResource->ReadPixels(ImageData);
}

void UVisionComponent::ReadLinearColor(UTextureRenderTarget2D *RenderTarget, TArray<FLinearColor> &ImageData) const
{
	FTextureRenderTargetResource *RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	RenderTargetResource->ReadLinearColorPixels(ImageData);
}

void UVisionComponent::ReadFloat16Color(UTextureRenderTarget2D *RenderTarget, TArray<FFloat16Color> &ImageData) const
{
	FTextureRenderTargetResource *RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	RenderTargetResource->ReadFloat16Pixels(ImageData);
}

void UVisionComponent::ReadImageCompressed(UTextureRenderTarget2D *RenderTarget, TArray<FFloat16Color> &ImageData) const
{
	TArray<FFloat16Color> RawImageData;
	FTextureRenderTargetResource *RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
	RenderTargetResource->ReadFloat16Pixels(RawImageData);

	static IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	static TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	ImageWrapper->SetRaw(RawImageData.GetData(), RawImageData.GetAllocatedSize(), Width, Height, ERGBFormat::BGRA, 8);
}

void UVisionComponent::ColorToBytes(const TArray<FColor> &ImageData, uint8 *Bytes) const
{
	const FColor *itI = ImageData.GetData();
	uint8_t *itO = Bytes;

	// Converts Float colors to bytes
	for (size_t i = 0; i < ImageData.Num(); ++i, ++itI, ++itO)
	{
		*itO = ProcessChannel(itI->B);
		*++itO = ProcessChannel(itI->G);
		*++itO = ProcessChannel(itI->R);
	}
	return;
}

void UVisionComponent::LinearColorToBytes(const TArray<FLinearColor> &ImageData, uint8 *Bytes) const
{
	const FLinearColor *itI = ImageData.GetData();
	uint8_t *itO = Bytes;

	// Converts Float colors to bytes
	for (size_t i = 0; i < ImageData.Num(); ++i, ++itI, ++itO)
	{
		*itO = ProcessChannel(itI->B);
		*++itO = ProcessChannel(itI->G);
		*++itO = ProcessChannel(itI->R);
	}
	return;
}

void UVisionComponent::Float16ColorToBytes(const TArray<FFloat16Color> &ImageData, uint8 *Bytes) const
{
	const FFloat16Color *itI = ImageData.GetData();
	uint8_t *itO = Bytes;

	// Converts Float colors to bytes
	for (size_t i = 0; i < ImageData.Num(); ++i, ++itI, ++itO)
	{
		*itO = ProcessChannel(itI->B);
		*++itO = ProcessChannel(itI->G);
		*++itO = ProcessChannel(itI->R);
	}
	return;
}

uint8_t UVisionComponent::ProcessChannel(const uint8 &channel) const
{
  // Apply gamma correction and brightness adjustments to the channel.
  float out = (FGenericPlatformMath::Pow(
    channel / 255.f, 1 / GammaCorrection) * 255.f);
  out += Contrast * (out - 128) + 128 + Brightness;
  // Clamp to range [0, 255]
  if (out > 255.f) out = 255.f;
  else if (out < 0.f) out = 0.f;
  return (uint8_t) std::round(out);
}

uint8_t UVisionComponent::ProcessChannel(const float &channel) const
{
  // Apply gamma correction and brightness adjustments to the channel.
  float out = (FGenericPlatformMath::Pow(
    channel / 255.f, 1 / GammaCorrection) * 255.f) * 255.f;
  out += Contrast * (out - 128) + 128 + Brightness;
  // Clamp to range [0, 255]
  if (out > 255.f) out = 255.f;
  else if (out < 0.f) out = 0.f;
  return (uint8_t) std::round(out);
}

uint8_t UVisionComponent::ProcessChannel(const FFloat16 &channel) const
{
  // Apply gamma correction and brightness adjustments to the channel.
  float out = (FGenericPlatformMath::Pow(
    channel / 255.f, 1 / GammaCorrection) * 255.f) * 255.f;
  out += Contrast * (out - 128) + 128 + Brightness;
  // Clamp to range [0, 255]
  if (out > 255.f) out = 255.f;
  else if (out < 0.f) out = 0.f;
  return (uint8_t) std::round(out);
}

void UVisionComponent::ToDepthImage(const TArray<FFloat16Color> &ImageData, uint8 *Bytes) const
{
	const FFloat16Color *itI = ImageData.GetData();
	uint16_t *itO = reinterpret_cast<uint16_t *>(Bytes);

	// Just copies the encoded Float16 values
	for (size_t i = 0; i < ImageData.Num(); ++i, ++itI, ++itO)
	{
		*itO = itI->R.Encoded;
	}
	return;
}

void UVisionComponent::StoreImage(const uint8 *ImageData, const uint32 Size, const char *Name) const
{
	std::ofstream File(Name, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
	File.write(reinterpret_cast<const char *>(ImageData), Size);
	File.close();
	return;
}

/* Generates at least NumberOfColors different colors.
 * It takes MaxHue different Hue values and additional steps ind Value and Saturation to get
 * the number of needed colors.
 */
void UVisionComponent::GenerateColors(const uint32_t NumberOfColors)
{
	const int32_t MaxHue = 50;
	// It shifts the next Hue value used, so that colors next to each other are not very similar. This is just important for humans
	const int32_t ShiftHue = 21;
	const float MinSat = 0.65;
	const float MinVal = 0.65;

	uint32_t HueCount = MaxHue;
	uint32_t SatCount = 1;
	uint32_t ValCount = 1;

	// Compute how many different Saturations and Values are needed
	int32_t left = std::max<int32_t>(0, NumberOfColors - HueCount);
	while (left > 0)
	{
		if (left > 0)
		{
			++ValCount;
			left = NumberOfColors - SatCount * ValCount * HueCount;
		}
		if (left > 0)
		{
			++SatCount;
			left = NumberOfColors - SatCount * ValCount * HueCount;
		}
	}

	const float StepHue = 360.0f / HueCount;
	const float StepSat = (1.0f - MinSat) / std::max(1.0f, SatCount - 1.0f);
	const float StepVal = (1.0f - MinVal) / std::max(1.0f, ValCount - 1.0f);

	ObjectColors.Reserve(SatCount * ValCount * HueCount);
	UE_LOG(LogTemp, Display, TEXT("Generating %d colors."), SatCount * ValCount * HueCount);

	FLinearColor HSVColor;
	for (uint32_t s = 0; s < SatCount; ++s)
	{
		HSVColor.G = 1.0f - s * StepSat;
		for (uint32_t v = 0; v < ValCount; ++v)
		{
			HSVColor.B = 1.0f - v * StepVal;
			for (uint32_t h = 0; h < HueCount; ++h)
			{
				HSVColor.R = ((h * ShiftHue) % MaxHue) * StepHue;
				ObjectColors.Add(HSVColor.HSVToLinearRGB().ToFColor(false));
				UE_LOG(LogTemp, Display, TEXT("Added color %d: %d %d %d"), ObjectColors.Num(), ObjectColors.Last().R, ObjectColors.Last().G, ObjectColors.Last().B);
			}
		}
	}
}

bool UVisionComponent::ColorObject(AActor *Actor, const FString &name)
{
	const FColor &ObjectColor = ObjectColors[ObjectToColor[name]];
	TArray<UMeshComponent *> PaintableComponents;
	Actor->GetComponents<UMeshComponent>(PaintableComponents);

	for (auto MeshComponent : PaintableComponents)
	{
		if (MeshComponent == nullptr)
			continue;

		if (UStaticMeshComponent *StaticMeshComponent = Cast<UStaticMeshComponent>(MeshComponent))
		{
			if (UStaticMesh *StaticMesh = StaticMeshComponent->GetStaticMesh())
			{
				uint32 PaintingMeshLODIndex = 0;
				uint32 NumLODLevel = StaticMesh->RenderData->LODResources.Num();
				//check(NumLODLevel == 1);
				FStaticMeshLODResources &LODModel = StaticMesh->RenderData->LODResources[PaintingMeshLODIndex];
				FStaticMeshComponentLODInfo *InstanceMeshLODInfo = NULL;

				// PaintingMeshLODIndex + 1 is the minimum requirement, enlarge if not satisfied
				StaticMeshComponent->SetLODDataCount(PaintingMeshLODIndex + 1, StaticMeshComponent->LODData.Num());
				InstanceMeshLODInfo = &StaticMeshComponent->LODData[PaintingMeshLODIndex];

				{
					InstanceMeshLODInfo->OverrideVertexColors = new FColorVertexBuffer;

					FColor FillColor = FColor(255, 255, 255, 255);
					InstanceMeshLODInfo->OverrideVertexColors->InitFromSingleColor(FColor::White, LODModel.GetNumVertices());
				}

				uint32 NumVertices = LODModel.GetNumVertices();

				for (uint32 ColorIndex = 0; ColorIndex < NumVertices; ++ColorIndex)
				{
					uint32 NumOverrideVertexColors = InstanceMeshLODInfo->OverrideVertexColors->GetNumVertices();
					uint32 NumPaintedVertices = InstanceMeshLODInfo->PaintedVertices.Num();
					InstanceMeshLODInfo->OverrideVertexColors->VertexColor(ColorIndex) = ObjectColor;
				}
				BeginInitResource(InstanceMeshLODInfo->OverrideVertexColors);
				StaticMeshComponent->MarkRenderStateDirty();
			}
		}
	}
	return true;
}

bool UVisionComponent::ColorAllObjects()
{
	uint32_t NumberOfActors = 0;

	for (TActorIterator<AActor> ActItr(GetWorld()); ActItr; ++ActItr)
	{
		++NumberOfActors;
		FString ActorName = ActItr->GetHumanReadableName();
		UE_LOG(LogTemp, Display, TEXT("Actor with name: %s."), *ActorName);
	}

	UE_LOG(LogTemp, Display, TEXT("Found %d Actors."), NumberOfActors);
	GenerateColors(NumberOfActors * 2);

	for (TActorIterator<AActor> ActItr(GetWorld()); ActItr; ++ActItr)
	{
		FString ActorName = ActItr->GetHumanReadableName();
		if (!ObjectToColor.Contains(ActorName))
		{
			check(ColorsUsed < (uint32)ObjectColors.Num());
			ObjectToColor.Add(ActorName, ColorsUsed);
			UE_LOG(LogTemp, Display, TEXT("Adding color %d for object %s."), ColorsUsed, *ActorName);

			++ColorsUsed;
		}

		UE_LOG(LogTemp, Display, TEXT("Coloring object %s."), *ActorName);
		ColorObject(*ActItr, ActorName);
	}

	return true;
}

void UVisionComponent::ProcessColor()
{
	while (this->Running)
	{
		std::unique_lock<std::mutex> WaitLock(Priv->WaitColor);
		Priv->CVColor.wait(WaitLock);
    // Process the image bytes based on the current format.
    switch (Format)
    {
      case EVisionFormat::LinearColor:
        LinearColorToBytes(ImageLinearColor, Priv->Buffer->Color);
        break;
      case EVisionFormat::Float16Color:
        Float16ColorToBytes(ImageFloat16Color, Priv->Buffer->Color);
        break;
      default:
        ColorToBytes(ImageColor, Priv->Buffer->Color);
        break;
    }
		Priv->CVColor.notify_one();
	}
}

void UVisionComponent::ProcessDepth()
{
	while (this->Running)
	{
		std::unique_lock<std::mutex> WaitLock(Priv->WaitDepth);
		Priv->CVDepth.wait(WaitLock);
		ToDepthImage(ImageDepth, Priv->Buffer->Depth);
        Priv->CVDepth.notify_one();
	}
}

// TODO maybe shift towards "server" who publishs async
void UVisionComponent::convertDepth(const uint16_t *in, __m128 *out) const
{
	const size_t size = (Width * Height) / 4;
	for (size_t i = 0; i < size; ++i, in += 4, ++out)
	{
    // Divide by 100 here in order to convert UU (cm) into ROS units (m)
		*out = _mm_cvtph_ps(_mm_set_epi16(
        0, 0, 0, 0, *(in + 3), *(in + 2), *(in + 1), *(in + 0))) / 100;
	}
}
