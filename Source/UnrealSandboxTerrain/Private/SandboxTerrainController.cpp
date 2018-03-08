
#include "UnrealSandboxTerrainPrivatePCH.h"
#include "SandboxTerrainController.h"
#include "TerrainZoneComponent.h"
#include "SandboxVoxeldata.h"
#include <cmath>
#include "DrawDebugHelpers.h"
#include "Async.h"
#include "Json.h"
#include "SandboxTerrainMeshComponent.h"


void LoadDataFromKvFile(const TKvFile& KvFile, const TVoxelIndex& Index, std::function<void(TArray<uint8>&)> Function);

void SerializeMeshData(TMeshData const * MeshDataPtr, TArray<uint8>& CompressedData);


class FAsyncThread : public FRunnable {

private:

	volatile int State = TH_STATE_NEW;

	FRunnableThread* Thread;

	std::function<void(FAsyncThread&)> Function;

public:

	FAsyncThread(std::function<void(FAsyncThread&)> Function) {
		Thread = NULL;
		this->Function = Function;
	}

	~FAsyncThread() {
		if (Thread != NULL) {
			delete Thread;
		}
	}

	bool IsFinished() {
		return State == TH_STATE_FINISHED;
	}

	bool IsNotValid() {
		if (State == TH_STATE_STOP) {
			State = TH_STATE_FINISHED;
			return true;
		}

		return false;
	}

	virtual void Stop() {
		if (State == TH_STATE_NEW || State == TH_STATE_RUNNING) {
			State = TH_STATE_STOP;
		}
	}

	virtual void WaitForFinish() {
		while (!IsFinished()) {

		}
	}

	void Start() {
		State = TH_STATE_RUNNING;
		Thread = FRunnableThread::Create(this, TEXT("THREAD_TEST"));

	}

	virtual uint32 Run() {
		Function(*this);
		
		State = TH_STATE_FINISHED;
		return 0;
	}
};


void TVoxelDataInfo::Unload() {
	if (Vd != nullptr) {
		delete Vd;
		Vd = nullptr;
	}

	DataState = TVoxelDataState::READY_TO_LOAD;
}


ASandboxTerrainController::ASandboxTerrainController(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {
	PrimaryActorTick.bCanEverTick = true;
	MapName = TEXT("World 0");
	TerrainSizeX = 5;
	TerrainSizeY = 5;
	TerrainSizeZ = 5;
	bEnableLOD = false;

	TerrainGeneratorComponent = CreateDefaultSubobject<UTerrainGeneratorComponent>(TEXT("TerrainGenerator"));
	TerrainGeneratorComponent->AttachTo(RootComponent);
}

ASandboxTerrainController::ASandboxTerrainController() {
	PrimaryActorTick.bCanEverTick = true;
	MapName = TEXT("World 0");
	TerrainSizeX = 5;
	TerrainSizeY = 5;
	TerrainSizeZ = 5;
	bEnableLOD = false;

	TerrainGeneratorComponent = CreateDefaultSubobject<UTerrainGeneratorComponent>(TEXT("TerrainGenerator"));
	TerrainGeneratorComponent->AttachTo(RootComponent);
}

void ASandboxTerrainController::PostLoad() {
	Super::PostLoad();

	UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainController ---> PostLoad"));

#if WITH_EDITOR
	//spawnInitialZone();
#endif

}

void ASandboxTerrainController::BeginPlay() {
	Super::BeginPlay();
	UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainController ---> BeginPlay"));

	if (!GetWorld()) return;

	if (GetWorld()->GetAuthGameMode() != NULL) {
		UE_LOG(LogTemp, Warning, TEXT("SERVER"));
	} else {
		UE_LOG(LogTemp, Warning, TEXT("CLIENT"));
	}

	if (!OpenFile()) return;

	UE_LOG(LogSandboxTerrain, Warning, TEXT("vd file ----> %d zones"), VdFile.size());

	//===========================
	// load existing
	//===========================
	OnStartBuildTerrain();
	bIsGeneratingTerrain = true;
	LoadJson();

	TSet<FVector> InitialZoneSet = SpawnInitialZone();
	// async loading other zones
	RunThread([&](FAsyncThread& ThisThread) {
		if (!bGenerateOnlySmallSpawnPoint) {
			int Total = (TerrainSizeX * 2 + 1) * (TerrainSizeY * 2 + 1) * (TerrainSizeZ * 2 + 1);
			int Progress = 0;

			for (int x = -TerrainSizeX; x <= TerrainSizeX; x++) {
				for (int y = -TerrainSizeY; y <= TerrainSizeY; y++) {
					for (int z = -TerrainSizeZ; z <= TerrainSizeZ; z++) {
						TVoxelIndex Index2(x, y, z);
						FVector Pos = GetZonePos(Index2);
						if (ThisThread.IsNotValid()) return;

						if (!HasVoxelData(Index2)) {
							SpawnZone(Pos);
						}

						Progress++;
						GeneratingProgress = (float)Progress / (float)Total;
						// must invoke in main thread
						//OnProgressBuildTerrain(PercentProgress);

						if (ThisThread.IsNotValid()) return;
					}
				}
			}
		}

		RunThread([&](FAsyncThread& ThisThread) {
			bIsGeneratingTerrain = false;
			OnFinishBuildTerrain(); // FIXME: thread-safe
		});
	});
}

void ASandboxTerrainController::EndPlay(const EEndPlayReason::Type EndPlayReason) {
	Super::EndPlay(EndPlayReason);

	// wait threads for finish
	for (auto* ThreadTask : ThreadList) {
		ThreadTask->Stop();
		ThreadTask->WaitForFinish();
	}

	// save only on server side
	if (GetWorld()->GetAuthGameMode() == NULL) {
		return;
	}

	Save();

	VdFile.close();
	MdFile.close();
	ObjFile.close();

	ClearVoxelData();
	TerrainZoneMap.Empty();
}

void ASandboxTerrainController::Tick(float DeltaTime) {
	Super::Tick(DeltaTime);

	if (HasNextAsyncTask()) {
		TerrainControllerTask task = GetAsyncTask();
		if (task.Function) {
			task.Function();
		}
	}	

	auto It = ThreadList.begin();
	while (It != ThreadList.end()) {
		FAsyncThread* ThreadPtr = *It;
		if (ThreadPtr->IsFinished()) {
			ThreadListMutex.lock();
			delete ThreadPtr;
			It = ThreadList.erase(It);
			ThreadListMutex.unlock();
		} else {
			It++;
		}
	}

	if (bIsGeneratingTerrain) {
		OnProgressBuildTerrain(GeneratingProgress);
	}
}

//======================================================================================================================================================================
// Unreal Sandbox 
//======================================================================================================================================================================

void ASandboxTerrainController::Save() {
	uint32 SavedVd = 0;
	uint32 SavedMd = 0;
	uint32 SavedObj = 0;

	for (auto& It : VoxelDataIndexMap) {
		TVoxelDataInfo& VdInfo = VoxelDataIndexMap[It.first];

		if (VdInfo.Vd == nullptr) {
			continue;
		}

		//============================================
		// TODO remove
		// test
		FBufferArchive TempBufferVd;
		TValueData buffer;

		serializeVoxelData(*VdInfo.Vd, TempBufferVd);
		buffer.reserve(TempBufferVd.Num());

		for (uint8 b : TempBufferVd) {
			buffer.push_back(b);
		}

		TVoxelIndex Index = GetZoneIndex(VdInfo.Vd->getOrigin());
		if (VdInfo.Vd->isChanged()) {
			VdFile.put(Index, buffer);
			VdInfo.Vd->resetLastSave();
			SavedVd++;
		}

		VdInfo.Unload();
	}
	UE_LOG(LogSandboxTerrain, Log, TEXT("Save voxel data ----> %d"), SavedVd);

	for (auto& Elem : TerrainZoneMap) {
		FVector ZoneIndex = Elem.Key;
		UTerrainZoneComponent* Zone = Elem.Value;

		TMeshData const * MeshDataPtr = Zone->GetCachedMeshData();
		if (MeshDataPtr) {
			TArray<uint8> TempBufferMd;
			SerializeMeshData(MeshDataPtr, TempBufferMd);

			TVoxelIndex Index2(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z);

			TValueData buffer;
			buffer.reserve(TempBufferMd.Num());
			for (uint8 b : TempBufferMd) {
				buffer.push_back(b);
			}

			MdFile.put(Index2, buffer);

			Zone->ClearCachedMeshData();
			SavedMd++;
		}

		if (Zone->IsNeedSave()) {
			FBufferArchive BinaryData;
			Zone->SerializeInstancedMeshes(BinaryData);

			TVoxelIndex Index2(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z);

			TValueData buffer;
			buffer.reserve(BinaryData.Num());
			for (uint8 b : BinaryData) {
				buffer.push_back(b);
			}

			ObjFile.put(Index2, buffer);

			Zone->ResetNeedSave();
			SavedObj++;
		}
	}
	UE_LOG(LogSandboxTerrain, Log, TEXT("Save mesh data ----> %d"), SavedMd);
	UE_LOG(LogSandboxTerrain, Log, TEXT("Save objects data ----> %d"), SavedObj);

	SaveJson();
}

void ASandboxTerrainController::SaveMapAsync() {
	UE_LOG(LogSandboxTerrain, Log, TEXT("Start save terrain async"));
	RunThread([&](FAsyncThread& ThisThread) {
		double Start = FPlatformTime::Seconds();
		Save();
		double End = FPlatformTime::Seconds();
		double Time = (End - Start) * 1000;
		UE_LOG(LogSandboxTerrain, Log, TEXT("Terrain saved -> %f ms"), Time);
	});
}

void ASandboxTerrainController::SaveJson() {
	UE_LOG(LogTemp, Log, TEXT("----------- save json -----------"));

	FString JsonStr;
	FString FileName = TEXT("terrain.json");
	FString SavePath = FPaths::GameSavedDir();
	FString FullPath = SavePath + TEXT("/Map/") + MapName + TEXT("/") + FileName;

	TSharedRef <TJsonWriter<TCHAR>> JsonWriter = TJsonWriterFactory<>::Create(&JsonStr);
	JsonWriter->WriteObjectStart();
	JsonWriter->WriteArrayStart("Regions");

	/*
	for (const FVector& Index : RegionIndexSet) {
		float x = Index.X;
		float y = Index.Y;
		float z = Index.Z;

		//FVector RegionPos = GetRegionPos(Index);
		FVector RegionPos;

		//========================================================
		JsonWriter->WriteObjectStart();
		JsonWriter->WriteObjectStart("Region");

		JsonWriter->WriteArrayStart("Index");
		JsonWriter->WriteValue(x);
		JsonWriter->WriteValue(y);
		JsonWriter->WriteValue(z);
		JsonWriter->WriteArrayEnd();

		JsonWriter->WriteArrayStart("Pos");
		JsonWriter->WriteValue(RegionPos.X);
		JsonWriter->WriteValue(RegionPos.Y);
		JsonWriter->WriteValue(RegionPos.Z);
		JsonWriter->WriteArrayEnd();

		JsonWriter->WriteObjectEnd();
		JsonWriter->WriteObjectEnd();
		//========================================================
	}
	*/

	JsonWriter->WriteArrayEnd();
	JsonWriter->WriteObjectEnd();
	JsonWriter->Close();

	FFileHelper::SaveStringToFile(*JsonStr, *FullPath);
}

bool OpenKvFile(kvdb::KvFile<TVoxelIndex, TValueData>& KvFile, const FString& FileName, const FString& SaveDir) {
	FString FullPath = SaveDir + FileName;
	std::string FilePathString = std::string(TCHAR_TO_UTF8(*FullPath));

	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath)) {
		kvdb::KvFile<TVoxelIndex, TValueData>::create(FilePathString, std::unordered_map<TVoxelIndex, TValueData>());// create new empty file
	}

	if (!KvFile.open(FilePathString)) {
		UE_LOG(LogSandboxTerrain, Warning, TEXT("Unable to open file: %s"), *FullPath);
		return false;
	}

	return true;
}

bool ASandboxTerrainController::OpenFile() {
	// open vd file 	
	FString FileNameVd = TEXT("terrain_voxeldata.dat");
	FString FileNameMd = TEXT("terrain_mesh.dat");
	FString FileNameObj = TEXT("terrain_objects.dat");

	FString SavePath = FPaths::GameSavedDir();
	FString SaveDir = SavePath + TEXT("/Map/") + MapName + TEXT("/");

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*SaveDir)) {
		PlatformFile.CreateDirectory(*SaveDir);
		if (!PlatformFile.DirectoryExists(*SaveDir)) {
			return false;
		}
	}

	if (!OpenKvFile(VdFile, FileNameVd, SaveDir)) {
		return false;
	}

	if (!OpenKvFile(MdFile, FileNameMd, SaveDir)) {
		return false;
	}

	if (!OpenKvFile(ObjFile, FileNameObj, SaveDir)) {
		return false;
	}

	return true;
}

void ASandboxTerrainController::LoadJson() {
	UE_LOG(LogTemp, Warning, TEXT("----------- load json -----------"));

	FString FileName = TEXT("terrain.json");
	FString SavePath = FPaths::GameSavedDir();
	FString FullPath = SavePath + TEXT("/Map/") + MapName + TEXT("/") + FileName;

	FString jsonRaw;
	if (!FFileHelper::LoadFileToString(jsonRaw, *FullPath, FFileHelper::EHashOptions::None)) {
		UE_LOG(LogTemp, Error, TEXT("Error loading json file"));
	}

	TSharedPtr<FJsonObject> jsonParsed;
	TSharedRef<TJsonReader<TCHAR>> jsonReader = TJsonReaderFactory<TCHAR>::Create(jsonRaw);
	if (FJsonSerializer::Deserialize(jsonReader, jsonParsed)) {

		TArray <TSharedPtr<FJsonValue>> array = jsonParsed->GetArrayField("Regions");
		for (int i = 0; i < array.Num(); i++) {
			TSharedPtr<FJsonObject> obj_ptr = array[i]->AsObject();
			TSharedPtr<FJsonObject> RegionObj = obj_ptr->GetObjectField(TEXT("Region"));

			TArray <TSharedPtr<FJsonValue>> IndexValArray = RegionObj->GetArrayField("Index");
			double x = IndexValArray[0]->AsNumber();
			double y = IndexValArray[1]->AsNumber();
			double z = IndexValArray[2]->AsNumber();
		}
	}
}

void ASandboxTerrainController::InvokeSafe(std::function<void()> Function) {
	if (IsInGameThread()) {
		Function();
	} else {
		TerrainControllerTask AsyncTask;
		AsyncTask.Function = Function;
		AddAsyncTask(AsyncTask);
	}
}

void ASandboxTerrainController::SpawnZone(const FVector& Pos) {
	TVoxelIndex ZoneIndex = GetZoneIndex(Pos);
	FVector ZoneIndexTmp(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z);

	if (GetZoneByVectorIndex(ZoneIndex) != nullptr) return;

	TMeshDataPtr MeshDataPtr = LoadMeshDataByIndex(ZoneIndex);
	if (MeshDataPtr != nullptr) {
		InvokeSafe([=]() {
			UTerrainZoneComponent* Zone = AddTerrainZone(Pos);
			Zone->ApplyTerrainMesh(MeshDataPtr, false); // already in cache
			OnLoadZone(Zone);
		});
		return;
	}

	TVoxelDataInfo* VoxelDataInfo = FindOrCreateZoneVoxeldata(ZoneIndex);
	if (VoxelDataInfo->Vd != nullptr && VoxelDataInfo->Vd->getDensityFillState() == TVoxelDataFillState::MIX) {
		InvokeSafe([=]() {
			UTerrainZoneComponent* Zone = AddTerrainZone(Pos);
			MakeTerrain(Zone, VoxelDataInfo->Vd);

			if (VoxelDataInfo->IsNewGenerated()) {
				OnGenerateNewZone(Zone);
			}

			if (VoxelDataInfo->IsNewLoaded()) {
				OnLoadZone(Zone);
			}
		});
	} else {
		if (VoxelDataInfo->IsNewGenerated()) {
			InvokeSafe([=]() {

			});
		}
	}
}


TSet<FVector> ASandboxTerrainController::SpawnInitialZone() {
	double start = FPlatformTime::Seconds();

	const int s = static_cast<int>(TerrainInitialArea);
	TSet<FVector> InitialZoneSet;

	if (s > 0) {
		for (auto x = -s; x <= s; x++) {
			for (auto y = -s; y <= s; y++) {
				for (auto z = -s; z <= s; z++) {
					FVector Pos = FVector((float)(x * USBT_ZONE_SIZE), (float)(y * USBT_ZONE_SIZE), (float)(z * USBT_ZONE_SIZE));
					SpawnZone(Pos);
					InitialZoneSet.Add(Pos);
				}
			}
		}
	} else {
		FVector Pos = FVector(0);
		SpawnZone(Pos);
		InitialZoneSet.Add(Pos);
	}	

	double end = FPlatformTime::Seconds();
	double time = (end - start) * 1000;

	return InitialZoneSet;
}

TVoxelIndex ASandboxTerrainController::GetZoneIndex(const FVector& Pos) {
	FVector Tmp = sandboxGridIndex(Pos, USBT_ZONE_SIZE);
	return TVoxelIndex(Tmp.X, Tmp.Y, Tmp.Z);
}

FVector ASandboxTerrainController::GetZonePos(const TVoxelIndex& Index) {
	return FVector((float)Index.X * USBT_ZONE_SIZE, (float)Index.Y * USBT_ZONE_SIZE, (float)Index.Z * USBT_ZONE_SIZE);
}

UTerrainZoneComponent* ASandboxTerrainController::GetZoneByVectorIndex(const TVoxelIndex& Index) {
	FVector TmpIndex(Index.X, Index.Y, Index.Z);
	if (TerrainZoneMap.Contains(TmpIndex)) {
		return TerrainZoneMap[TmpIndex];
	}

	return NULL;
}

UTerrainZoneComponent* ASandboxTerrainController::AddTerrainZone(FVector pos) {
	TVoxelIndex Index = GetZoneIndex(pos);
	FVector IndexTmp(Index.X, Index.Y,Index.Z);

	FString ZoneName = FString::Printf(TEXT("Zone -> [%.0f, %.0f, %.0f]"), IndexTmp.X, IndexTmp.Y, IndexTmp.Z);
	UTerrainZoneComponent* ZoneComponent = NewObject<UTerrainZoneComponent>(this, FName(*ZoneName));
	if (ZoneComponent) {
		ZoneComponent->RegisterComponent();
		//ZoneComponent->SetRelativeLocation(pos);
		ZoneComponent->AttachTo(RootComponent);
		ZoneComponent->SetWorldLocation(pos);

		FString TerrainMeshCompName = FString::Printf(TEXT("TerrainMesh -> [%.0f, %.0f, %.0f]"), IndexTmp.X, IndexTmp.Y, IndexTmp.Z);
		USandboxTerrainMeshComponent* TerrainMeshComp = NewObject<USandboxTerrainMeshComponent>(this, FName(*TerrainMeshCompName));
		TerrainMeshComp->RegisterComponent();
		TerrainMeshComp->SetMobility(EComponentMobility::Stationary);
		TerrainMeshComp->AttachTo(ZoneComponent);

		FString CollisionMeshCompName = FString::Printf(TEXT("CollisionMesh -> [%.0f, %.0f, %.0f]"), IndexTmp.X, IndexTmp.Y, IndexTmp.Z);
		USandboxTerrainCollisionComponent* CollisionMeshComp = NewObject<USandboxTerrainCollisionComponent>(this, FName(*CollisionMeshCompName));
		CollisionMeshComp->RegisterComponent();
		CollisionMeshComp->SetMobility(EComponentMobility::Stationary);
		CollisionMeshComp->SetCanEverAffectNavigation(true);
		CollisionMeshComp->SetCollisionProfileName(TEXT("InvisibleWall"));
		CollisionMeshComp->AttachTo(ZoneComponent);

		ZoneComponent->MainTerrainMesh = TerrainMeshComp;
		ZoneComponent->CollisionMesh = CollisionMeshComp;
	}

	TerrainZoneMap.Add(IndexTmp, ZoneComponent);
	if(bShowZoneBounds) DrawDebugBox(GetWorld(), pos, FVector(USBT_ZONE_SIZE / 2), FColor(255, 0, 0, 100), true);
	return ZoneComponent;
}

//======================================================================================================================================================================
// Edit Terrain
//======================================================================================================================================================================

template<class H>
class FTerrainEditThread : public FRunnable {
public:
	H zone_handler;
	FVector origin;
	float radius;
	ASandboxTerrainController* instance;

	virtual uint32 Run() {
		instance->EditTerrain(origin, radius, zone_handler);
		return 0;
	}
};

struct TZoneEditHandler {
	bool changed = false;
	bool enableLOD = false;
	float Strength;
};

void ASandboxTerrainController::FillTerrainRound(const FVector origin, const float r, const int matId) {
	struct ZoneHandler : TZoneEditHandler {
		int newMaterialId;
		bool operator()(TVoxelData* vd, FVector v, float radius) {
			changed = false;

			vd->forEachWithCache([&](int x, int y, int z) {
				float density = vd->getDensity(x, y, z);
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= v;

				float rl = std::sqrt(o.X * o.X + o.Y * o.Y + o.Z * o.Z);
				if (rl < radius) {
					//2^-((x^2)/20)
					float d = density + 1 / rl * Strength;
					vd->setDensity(x, y, z, d);
					changed = true;
				}

				if (rl < radius + 20) {
					vd->setMaterial(x, y, z, newMaterialId);
				}			
			}, enableLOD);

			return changed;
		}
	} zh;

	zh.newMaterialId = matId;
	zh.enableLOD = bEnableLOD;
	zh.Strength = 5;
	ASandboxTerrainController::PerformTerrainChange(origin, r, zh);
}


void ASandboxTerrainController::DigTerrainRoundHole(FVector origin, float r, float strength) {

	struct ZoneHandler : TZoneEditHandler {
		TMap<uint16, FSandboxTerrainMaterial>* MaterialMapPtr;

		bool operator()(TVoxelData* vd, FVector v, float radius) {
			changed = false;
			
			vd->forEachWithCache([&](int x, int y, int z) {
				float density = vd->getDensity(x, y, z);
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= v;

				float rl = std::sqrt(o.X * o.X + o.Y * o.Y + o.Z * o.Z);
				if (rl < radius) {
					unsigned short  MatId = vd->getMaterial(x, y, z);
					FSandboxTerrainMaterial& Mat = MaterialMapPtr->FindOrAdd(MatId);

					float ClcStrength = (Mat.RockHardness == 0) ? Strength : (Strength / Mat.RockHardness);
					if (ClcStrength > 0.1) {
						float d = density - 1 / rl * (ClcStrength);
						vd->setDensity(x, y, z, d);
					}

					changed = true;
				}
			}, enableLOD);

			return changed;
		}
	} zh;

	zh.MaterialMapPtr = &MaterialMap;
	zh.enableLOD = bEnableLOD;
	zh.Strength = strength;
	ASandboxTerrainController::PerformTerrainChange(origin, r, zh);
}

void ASandboxTerrainController::DigTerrainCubeHole(FVector origin, float r, float strength) {

	struct ZoneHandler : TZoneEditHandler {
		TMap<uint16, FSandboxTerrainMaterial>* MaterialMapPtr;

		bool operator()(TVoxelData* vd, FVector v, float radius) {
			changed = false;

			vd->forEachWithCache([&](int x, int y, int z) {
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= v;
				if (o.X < radius && o.X > -radius && o.Y < radius && o.Y > -radius && o.Z < radius && o.Z > -radius) {
					unsigned short  MatId = vd->getMaterial(x, y, z);
					FSandboxTerrainMaterial& Mat = MaterialMapPtr->FindOrAdd(MatId);
					if (Mat.RockHardness < 100) {
						vd->setDensity(x, y, z, 0);
						changed = true;
					}
				}
			}, enableLOD);

			return changed;
		}
	} zh;

	zh.enableLOD = bEnableLOD;
	zh.MaterialMapPtr = &MaterialMap;
	ASandboxTerrainController::PerformTerrainChange(origin, r, zh);
}

void ASandboxTerrainController::FillTerrainCube(FVector origin, const float r, const int matId) {

	struct ZoneHandler : TZoneEditHandler {
		int newMaterialId;
		bool operator()(TVoxelData* vd, FVector v, float radius) {
			changed = false;

			vd->forEachWithCache([&](int x, int y, int z) {
				FVector o = vd->voxelIndexToVector(x, y, z);
				o += vd->getOrigin();
				o -= v;
				if (o.X < radius && o.X > -radius && o.Y < radius && o.Y > -radius && o.Z < radius && o.Z > -radius) {
					vd->setDensity(x, y, z, 1);
					changed = true;
				}

				float radiusMargin = radius + 20;
				if (o.X < radiusMargin && o.X > -radiusMargin && o.Y < radiusMargin && o.Y > -radiusMargin && o.Z < radiusMargin && o.Z > -radiusMargin) {
					vd->setMaterial(x, y, z, newMaterialId);
				}
			}, enableLOD);

			return changed;
		}
	} zh;

	zh.newMaterialId = matId;
	zh.enableLOD = bEnableLOD;
	ASandboxTerrainController::PerformTerrainChange(origin, r, zh);
}

template<class H>
void ASandboxTerrainController::PerformTerrainChange(FVector Origin, float Radius, H Handler) {
	FTerrainEditThread<H>* EditThread = new FTerrainEditThread<H>();
	EditThread->zone_handler = Handler;
	EditThread->origin = Origin;
	EditThread->radius = Radius;
	EditThread->instance = this;

	FString ThreadName = FString::Printf(TEXT("terrain_change-thread-%d"), FPlatformTime::Seconds());
	FRunnableThread* Thread = FRunnableThread::Create(EditThread, *ThreadName);
	//FIXME delete thread after finish

	FVector TestPoint(Origin);
	TestPoint.Z -= 10;
	TArray<struct FHitResult> OutHits;
	bool bIsOverlap = GetWorld()->SweepMultiByChannel(OutHits, Origin, TestPoint, FQuat(), ECC_Visibility, FCollisionShape::MakeSphere(Radius)); // ECC_Visibility
	if (bIsOverlap) {
		for (auto OverlapItem : OutHits) {
			AActor* Actor = OverlapItem.GetActor();
			if (Cast<ASandboxTerrainController>(OverlapItem.GetActor()) != nullptr) {
				UHierarchicalInstancedStaticMeshComponent* InstancedMesh = Cast<UHierarchicalInstancedStaticMeshComponent>(OverlapItem.GetComponent());
				if (InstancedMesh != nullptr) {
					InstancedMesh->RemoveInstance(OverlapItem.Item);

					TArray<USceneComponent*> Parents;
					InstancedMesh->GetParentComponents(Parents);
					if (Parents.Num() > 0) {
						UTerrainZoneComponent* Zone = Cast<UTerrainZoneComponent>(Parents[0]);
						if (Zone) {
							Zone->SetNeedSave();
						}
					}
				}
			}
		}
	}
}

template<class H>
void ASandboxTerrainController::EditTerrain(FVector v, float radius, H handler) {
	double Start = FPlatformTime::Seconds();

	static float ZoneVolumeSize = USBT_ZONE_SIZE / 2;
	TVoxelIndex BaseZoneIndex = GetZoneIndex(v);

	static const float V[3] = { -1, 0, 1 };
	for (float x : V) {
		for (float y : V) {
			for (float z : V) {
				TVoxelIndex ZoneIndex = BaseZoneIndex + TVoxelIndex(x, y, z);;
				UTerrainZoneComponent* Zone = GetZoneByVectorIndex(ZoneIndex);
				TVoxelData* VoxelData = GetVoxelDataByIndex(ZoneIndex);

				// check zone bounds
				FVector ZoneOrigin = GetZonePos(ZoneIndex);
				FVector Upper(ZoneOrigin.X + ZoneVolumeSize, ZoneOrigin.Y + ZoneVolumeSize, ZoneOrigin.Z + ZoneVolumeSize);
				FVector Lower(ZoneOrigin.X - ZoneVolumeSize, ZoneOrigin.Y - ZoneVolumeSize, ZoneOrigin.Z - ZoneVolumeSize);

				if (FMath::SphereAABBIntersection(FSphere(v, radius), FBox(Lower, Upper))) {
					if (Zone == NULL) {
						if (VoxelData != NULL) {
							VoxelData->vd_edit_mutex.lock();
							bool bIsChanged = handler(VoxelData, v, radius);
							if (bIsChanged) {
								VoxelData->setChanged();
								VoxelData->vd_edit_mutex.unlock();
								InvokeLazyZoneAsync(FVector(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z));
							} else {
								VoxelData->vd_edit_mutex.unlock();
							}
							continue;
						} else {
							continue;
						}
					}

					if (VoxelData == NULL) {
						//try to load lazy voxel data
						VoxelData = LoadVoxelDataByIndex(ZoneIndex);

						if (VoxelData == nullptr) {
							continue;
						}
					}

					VoxelData->vd_edit_mutex.lock();
					bool bIsChanged = handler(VoxelData, v, radius);
					if (bIsChanged) {
						VoxelData->setChanged();
						VoxelData->setCacheToValid();
						TMeshDataPtr MeshDataPtr = GenerateMesh(Zone, VoxelData);
						VoxelData->resetLastMeshRegenerationTime();
						VoxelData->vd_edit_mutex.unlock();
						InvokeZoneMeshAsync(Zone, MeshDataPtr);
					} else {
						VoxelData->vd_edit_mutex.unlock();
					}
				}
			}
		}
	}

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;
	UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainController::editTerrain-------------> %f %f %f --> %f ms"), v.X, v.Y, v.Z, Time);
}


void ASandboxTerrainController::InvokeZoneMeshAsync(UTerrainZoneComponent* Zone, TMeshDataPtr MeshDataPtr) {
	TerrainControllerTask task;
	task.Function = [=]() {
		if (MeshDataPtr) {
			Zone->ApplyTerrainMesh(MeshDataPtr);
		}
	};

	AddAsyncTask(task);
}

void ASandboxTerrainController::InvokeLazyZoneAsync(FVector ZoneIndex) {
	TerrainControllerTask Task;

	FVector Pos = GetZonePos(TVoxelIndex(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z));
	TVoxelData* VoxelData = GetVoxelDataByIndex(TVoxelIndex(ZoneIndex.X, ZoneIndex.Y, ZoneIndex.Z));

	if (VoxelData == nullptr) {
		return;
	}

	Task.Function = [=]() {	
		if(VoxelData != nullptr) {
			UTerrainZoneComponent* Zone = AddTerrainZone(Pos);
			TMeshDataPtr NewMeshDataPtr = GenerateMesh(Zone, VoxelData);
			VoxelData->resetLastMeshRegenerationTime();
			Zone->ApplyTerrainMesh(NewMeshDataPtr);
		}
	};

	AddAsyncTask(Task);
}

//======================================================================================================================================================================


TVoxelData* ASandboxTerrainController::LoadVoxelDataByIndex(const TVoxelIndex& Index) {
	double Start = FPlatformTime::Seconds();

	TVoxelData* Vd = new TVoxelData(USBT_ZONE_DIMENSION, USBT_ZONE_SIZE);
	Vd->setOrigin(GetZonePos(Index));

	LoadDataFromKvFile(VdFile, Index, [&](TArray<uint8>& Data) { 
		FMemoryReader BinaryData = FMemoryReader(Data, true); //true, free data after done
		BinaryData.Seek(0);
		deserializeVoxelData(*Vd, BinaryData);
	});

	TVoxelDataInfo VdInfo;
	VdInfo.DataState = TVoxelDataState::LOADED;
	VdInfo.Vd = Vd;

	RegisterTerrainVoxelData(VdInfo, Index);

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	UE_LOG(LogTemp, Log, TEXT("loading voxel data block -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time);

	return Vd;
}

TVoxelDataInfo* ASandboxTerrainController::FindOrCreateZoneVoxeldata(const TVoxelIndex& Index) {
	if (HasVoxelData(Index)) {
		return GetVoxelDataInfo(Index);
	} else {
		if (VdFile.get(Index) != nullptr) {
			TVoxelDataInfo VdInfo;
			VdInfo.DataState = TVoxelDataState::READY_TO_LOAD;

			RegisterTerrainVoxelData(VdInfo, Index);
			return &VoxelDataIndexMap[Index];
		} else {
			TVoxelDataInfo VdInfo;
			TVoxelData* Vd = GetVoxelDataByIndex(Index);
			Vd = new TVoxelData(USBT_ZONE_DIMENSION, USBT_ZONE_SIZE);
			Vd->setOrigin(GetZonePos(Index));

			TerrainGeneratorComponent->GenerateVoxelTerrain(*Vd);
			GeneratedVdConter++;

			VdInfo.DataState = TVoxelDataState::GENERATED;
			VdInfo.Vd = Vd;

			Vd->setChanged();
			Vd->setCacheToValid();

			RegisterTerrainVoxelData(VdInfo, Index);
			return &VoxelDataIndexMap[Index];
		}
	}
}

void ASandboxTerrainController::OnGenerateNewZone(UTerrainZoneComponent* Zone) {
	if (!bDisableFoliage) {
		TerrainGeneratorComponent->GenerateNewFoliage(Zone);
	}
}

void ASandboxTerrainController::OnLoadZone(UTerrainZoneComponent* Zone) {
	if (!bDisableFoliage) {
		LoadFoliage(Zone);
	}
}

void ASandboxTerrainController::AddAsyncTask(TerrainControllerTask zone_make_task) {
	AsyncTaskListMutex.lock();
	AsyncTaskList.push(zone_make_task);
	AsyncTaskListMutex.unlock();
}

TerrainControllerTask ASandboxTerrainController::GetAsyncTask() {
	AsyncTaskListMutex.lock();
	TerrainControllerTask NewTask = AsyncTaskList.front();
	AsyncTaskList.pop();
	AsyncTaskListMutex.unlock();

	return NewTask;
}

bool ASandboxTerrainController::HasNextAsyncTask() {
	return AsyncTaskList.size() > 0;
}

void ASandboxTerrainController::RegisterTerrainVoxelData(TVoxelDataInfo VdInfo, TVoxelIndex Index) {
	VoxelDataMapMutex.lock();
	auto It = VoxelDataIndexMap.find(Index);
	if (It != VoxelDataIndexMap.end()) {
		VoxelDataIndexMap.erase(It);
	}
	VoxelDataIndexMap.insert({ Index, VdInfo });
	VoxelDataMapMutex.unlock();
}

void ASandboxTerrainController::RunThread(std::function<void(FAsyncThread&)> Function) {
	FAsyncThread* ThreadTask = new FAsyncThread(Function);
	ThreadListMutex.lock();
	ThreadList.push_back(ThreadTask);
	ThreadTask->Start();
	ThreadListMutex.unlock();
}

TVoxelData* ASandboxTerrainController::GetVoxelDataByPos(const FVector& Pos) {
	return GetVoxelDataByIndex(GetZoneIndex(Pos));
}

TVoxelData* ASandboxTerrainController::GetVoxelDataByIndex(const TVoxelIndex& Index) {
	VoxelDataMapMutex.lock();
	if (VoxelDataIndexMap.find(Index) != VoxelDataIndexMap.end()) {
		TVoxelDataInfo VdInfo = VoxelDataIndexMap[Index];
		VoxelDataMapMutex.unlock();
		return VdInfo.Vd;
	}

	VoxelDataMapMutex.unlock();
	return NULL;
}

bool ASandboxTerrainController::HasVoxelData(const TVoxelIndex& Index) const {
		return VoxelDataIndexMap.find(Index) != VoxelDataIndexMap.end();
}

TVoxelDataInfo* ASandboxTerrainController::GetVoxelDataInfo(const TVoxelIndex& Index) {
	if (VoxelDataIndexMap.find(Index) != VoxelDataIndexMap.end()) {
		return &VoxelDataIndexMap[Index];
	}

	return nullptr;
}

void ASandboxTerrainController::ClearVoxelData() {
	VoxelDataIndexMap.clear();
}

//======================================================================================================================================================================
// Sandbox Foliage
//======================================================================================================================================================================

void ASandboxTerrainController::LoadFoliage(UTerrainZoneComponent* Zone) {
	if (bDisableFoliage) return;

	TInstMeshTypeMap ZoneInstMeshMap;
	LoadObjectDataByIndex(Zone, ZoneInstMeshMap);

	for (auto& Elem : ZoneInstMeshMap) {
		TInstMeshTransArray& InstMeshTransArray = Elem.Value;

		for (FTransform& Transform : InstMeshTransArray.TransformArray) {
			Zone->SpawnInstancedMesh(InstMeshTransArray.MeshType, Transform);
		}
	}
}

//======================================================================================================================================================================
// Materials
//======================================================================================================================================================================

UMaterialInterface* ASandboxTerrainController::GetRegularTerrainMaterial(uint16 MaterialId) {
	if (RegularMaterial == nullptr) {
		return nullptr;
	}

	if (!RegularMaterialCache.Contains(MaterialId)) {
		UE_LOG(LogTemp, Warning, TEXT("create new regular terrain material instance ----> id: %d"), MaterialId);

		UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(RegularMaterial, this);

		if (MaterialMap.Contains(MaterialId)) {
			FSandboxTerrainMaterial Mat = MaterialMap[MaterialId];

			DynMaterial->SetTextureParameterValue("TextureTopMicro", Mat.TextureTopMicro);
			//DynMaterial->SetTextureParameterValue("TextureSideMicro", Mat.TextureSideMicro);
			DynMaterial->SetTextureParameterValue("TextureMacro", Mat.TextureMacro);
			DynMaterial->SetTextureParameterValue("TextureNormal", Mat.TextureNormal);
		}

		RegularMaterialCache.Add(MaterialId, DynMaterial);
		return DynMaterial;
	}

	return RegularMaterialCache[MaterialId];
}

UMaterialInterface* ASandboxTerrainController::GetTransitionTerrainMaterial(FString& TransitionName, std::set<unsigned short>& MaterialIdSet) {
	if (TransitionMaterial == nullptr) {
		return nullptr;
	}

	if (!TransitionMaterialCache.Contains(TransitionName)) {
		UE_LOG(LogTemp, Warning, TEXT("create new transition terrain material instance ----> id: %s"), *TransitionName);

		UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(TransitionMaterial, this);

		int Idx = 0;
		for (unsigned short MatId : MaterialIdSet) {
			if (MaterialMap.Contains(MatId)) {
				FSandboxTerrainMaterial Mat = MaterialMap[MatId];

				FName TextureTopMicroParam = FName(*FString::Printf(TEXT("TextureTopMicro%d"), Idx));
				//FName TextureSideMicroParam = FName(*FString::Printf(TEXT("TextureSideMicro%d"), Idx));
				FName TextureMacroParam = FName(*FString::Printf(TEXT("TextureMacro%d"), Idx));
				FName TextureNormalParam = FName(*FString::Printf(TEXT("TextureNormal%d"), Idx));

				DynMaterial->SetTextureParameterValue(TextureTopMicroParam, Mat.TextureTopMicro);
				//DynMaterial->SetTextureParameterValue(TextureSideMicroParam, Mat.TextureSideMicro);
				DynMaterial->SetTextureParameterValue(TextureMacroParam, Mat.TextureMacro);
				DynMaterial->SetTextureParameterValue(TextureNormalParam, Mat.TextureNormal);
			}

			Idx++;
		}

		TransitionMaterialCache.Add(TransitionName, DynMaterial);
		return DynMaterial;
	}

	return TransitionMaterialCache[TransitionName];
}

float ASandboxTerrainController::GetRealGroungLevel(float X, float Y) {
	return TerrainGeneratorComponent->GroundLevelFunc(FVector(X, Y, 0)); ;
}

//======================================================================================================================================================================
// mesh data de/serealization
//======================================================================================================================================================================

void ASandboxTerrainController::MakeTerrain(UTerrainZoneComponent* Zone, TVoxelData* Vd) {
	std::shared_ptr<TMeshData> MeshDataPtr = GenerateMesh(Zone, Vd);

	if (IsInGameThread()) {
		Zone->ApplyTerrainMesh(MeshDataPtr);
		Vd->resetLastMeshRegenerationTime();
	} else {
		UE_LOG(LogTemp, Warning, TEXT("non-game thread -> invoke async task"));
		InvokeZoneMeshAsync(Zone, MeshDataPtr);
	}
}

std::shared_ptr<TMeshData> ASandboxTerrainController::GenerateMesh(UTerrainZoneComponent* Zone, TVoxelData* Vd) {
	double start = FPlatformTime::Seconds();

	if (Vd == NULL || Vd->getDensityFillState() == TVoxelDataFillState::ZERO ||	Vd->getDensityFillState() == TVoxelDataFillState::ALL) {
		return NULL;
	}

	TVoxelDataParam Vdp;

	if (bEnableLOD) {
		Vdp.bGenerateLOD = true;
		Vdp.collisionLOD = GetCollisionMeshSectionLodIndex();
	} else {
		Vdp.bGenerateLOD = false;
		Vdp.collisionLOD = 0;
	}

	TMeshDataPtr MeshDataPtr = sandboxVoxelGenerateMesh(*Vd, Vdp);

	double end = FPlatformTime::Seconds();
	double time = (end - start) * 1000;

	//UE_LOG(LogTemp, Warning, TEXT("ASandboxTerrainZone::generateMesh -------------> %f %f %f --> %f ms"), GetComponentLocation().X, GetComponentLocation().Y, GetComponentLocation().Z, time);

	return MeshDataPtr;
}

//======================================================================================================================================================================
// mesh data de/serealization
//======================================================================================================================================================================

void SerializeMeshContainer(const TMeshContainer& MeshContainer, FBufferArchive& BinaryData) {
	// save regular materials
	int32 LodSectionRegularMatNum = MeshContainer.MaterialSectionMap.Num();
	BinaryData << LodSectionRegularMatNum;
	for (auto& Elem : MeshContainer.MaterialSectionMap) {
		unsigned short MatId = Elem.Key;
		const TMeshMaterialSection& MaterialSection = Elem.Value;

		BinaryData << MatId;

		const FProcMeshSection& Mesh = MaterialSection.MaterialMesh;
		Mesh.SerializeMesh(BinaryData);
	}

	// save transition materials
	int32 LodSectionTransitionMatNum = MeshContainer.MaterialTransitionSectionMap.Num();
	BinaryData << LodSectionTransitionMatNum;
	for (auto& Elem : MeshContainer.MaterialTransitionSectionMap) {
		unsigned short MatId = Elem.Key;
		const TMeshMaterialTransitionSection& TransitionMaterialSection = Elem.Value;

		BinaryData << MatId;

		int MatSetSize = TransitionMaterialSection.MaterialIdSet.size();
		BinaryData << MatSetSize;

		for (unsigned short MatSetElement : TransitionMaterialSection.MaterialIdSet) {
			BinaryData << MatSetElement;
		}

		const FProcMeshSection& Mesh = TransitionMaterialSection.MaterialMesh;
		Mesh.SerializeMesh(BinaryData);
	}
}

void SerializeMeshData(TMeshData const * MeshDataPtr, TArray<uint8>& CompressedData) {
	FBufferArchive BinaryData;

	int32 LodArraySize = MeshDataPtr->MeshSectionLodArray.Num();
	BinaryData << LodArraySize;

	for (int32 LodIdx = 0; LodIdx < LodArraySize; LodIdx++) {
		const TMeshLodSection& LodSection = MeshDataPtr->MeshSectionLodArray[LodIdx];
		BinaryData << LodIdx;

		// save whole mesh
		LodSection.WholeMesh.SerializeMesh(BinaryData);

		SerializeMeshContainer(LodSection.RegularMeshContainer, BinaryData);

		if (LodIdx > 0) {
			for (auto i = 0; i < 6; i++) {
				SerializeMeshContainer(LodSection.TransitionPatchArray[i], BinaryData);
			}
		}
	}

	FArchiveSaveCompressedProxy Compressor = FArchiveSaveCompressedProxy(CompressedData, ECompressionFlags::COMPRESS_ZLIB);
	Compressor << BinaryData;
	Compressor.Flush();
}

void DeserializeMeshContainer(TMeshContainer& MeshContainer, FMemoryReader& BinaryData) {
	// regular materials
	int32 LodSectionRegularMatNum;
	BinaryData << LodSectionRegularMatNum;

	for (int RMatIdx = 0; RMatIdx < LodSectionRegularMatNum; RMatIdx++) {
		unsigned short MatId;
		BinaryData << MatId;

		TMeshMaterialSection& MatSection = MeshContainer.MaterialSectionMap.FindOrAdd(MatId);
		MatSection.MaterialId = MatId;

		MatSection.MaterialMesh.DeserializeMesh(BinaryData);
	}

	// transition materials
	int32 LodSectionTransitionMatNum;
	BinaryData << LodSectionTransitionMatNum;

	for (int TMatIdx = 0; TMatIdx < LodSectionTransitionMatNum; TMatIdx++) {
		unsigned short MatId;
		BinaryData << MatId;

		int MatSetSize;
		BinaryData << MatSetSize;

		std::set<unsigned short> MatSet;
		for (int MatSetIdx = 0; MatSetIdx < MatSetSize; MatSetIdx++) {
			unsigned short MatSetElement;
			BinaryData << MatSetElement;

			MatSet.insert(MatSetElement);
		}

		TMeshMaterialTransitionSection& MatTransSection = MeshContainer.MaterialTransitionSectionMap.FindOrAdd(MatId);
		MatTransSection.MaterialId = MatId;
		MatTransSection.MaterialIdSet = MatSet;
		MatTransSection.TransitionName = TMeshMaterialTransitionSection::GenerateTransitionName(MatSet);

		MatTransSection.MaterialMesh.DeserializeMesh(BinaryData);
	}
}

TMeshDataPtr DeserializeMeshData(FMemoryReader& BinaryData, uint32 CollisionMeshSectionLodIndex) {
	TMeshDataPtr MeshDataPtr(new TMeshData);

	int32 LodArraySize;
	BinaryData << LodArraySize;

	for (int LodIdx = 0; LodIdx < LodArraySize; LodIdx++) {
		int32 LodIndex;
		BinaryData << LodIndex;

		// whole mesh
		MeshDataPtr.get()->MeshSectionLodArray[LodIndex].WholeMesh.DeserializeMesh(BinaryData);
		DeserializeMeshContainer(MeshDataPtr.get()->MeshSectionLodArray[LodIndex].RegularMeshContainer, BinaryData);

		if (LodIdx > 0) {
			for (auto i = 0; i < 6; i++) {
				DeserializeMeshContainer(MeshDataPtr.get()->MeshSectionLodArray[LodIndex].TransitionPatchArray[i], BinaryData);
			}
		}
	}

	MeshDataPtr.get()->CollisionMeshPtr = &MeshDataPtr.get()->MeshSectionLodArray[CollisionMeshSectionLodIndex].WholeMesh;//GetTerrainController()->GetCollisionMeshSectionLodIndex()
	return MeshDataPtr;
}

void LoadDataFromKvFile(const TKvFile& KvFile, const TVoxelIndex& Index, std::function<void(TArray<uint8>&)> Function) {
	std::shared_ptr<TValueData> DataPtr = KvFile.get(Index);

	if (DataPtr == nullptr || DataPtr->size() == 0) {
		return;
	}

	//TODO optimize all
	TArray<uint8> BinaryArray;
	BinaryArray.Reserve(DataPtr->size());

	TValueData& Data = *DataPtr.get();

	for (auto Byte : Data) {
		BinaryArray.Add(Byte);
	}

	Function(BinaryArray);
	return;
}


TMeshDataPtr ASandboxTerrainController::LoadMeshDataByIndex(const TVoxelIndex& Index) {
	double Start = FPlatformTime::Seconds();
	TMeshDataPtr MeshDataPtr = nullptr;

	LoadDataFromKvFile(MdFile, Index, [&](TArray<uint8>& Data) {
		FArchiveLoadCompressedProxy Decompressor = FArchiveLoadCompressedProxy(Data, ECompressionFlags::COMPRESS_ZLIB);
		if (Decompressor.GetError()) {
			UE_LOG(LogTemp, Log, TEXT("FArchiveLoadCompressedProxy -> ERROR : File was not compressed"));
			return;
		}

		FBufferArchive DecompressedBinaryArray;
		Decompressor << DecompressedBinaryArray;

		FMemoryReader BinaryData = FMemoryReader(DecompressedBinaryArray, true); //true, free data after done
		BinaryData.Seek(0);

		MeshDataPtr = DeserializeMeshData(BinaryData, GetCollisionMeshSectionLodIndex());

		Data.Empty();
		Decompressor.FlushCache();
		BinaryData.FlushCache();
		DecompressedBinaryArray.Empty();
		DecompressedBinaryArray.Close();
	});

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	UE_LOG(LogTemp, Log, TEXT("loading mesh data block -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time);

	return MeshDataPtr;
}

void ASandboxTerrainController::LoadObjectDataByIndex(UTerrainZoneComponent* Zone, TInstMeshTypeMap& ZoneInstMeshMap) {
	double Start = FPlatformTime::Seconds();
	TVoxelIndex Index = GetZoneIndex(Zone->GetComponentLocation());

	LoadDataFromKvFile(ObjFile, Index, [&](TArray<uint8>& Data) {
		FMemoryReader BinaryData = FMemoryReader(Data, true); //true, free data after done
		BinaryData.Seek(0);

		Zone->DeserializeInstancedMeshes(BinaryData, ZoneInstMeshMap);
	});

	double End = FPlatformTime::Seconds();
	double Time = (End - Start) * 1000;

	UE_LOG(LogTemp, Log, TEXT("loading inst-objects data block -> %d %d %d -> %f ms"), Index.X, Index.Y, Index.Z, Time);
}

