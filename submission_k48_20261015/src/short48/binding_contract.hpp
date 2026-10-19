#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace srb_v11_short48 {
inline constexpr unsigned Sources=110, Targets=256, Old=32, Added=16, Basis=48;
inline constexpr unsigned Side=145, Cells=21025, Groups=480, DomainCells=7704;
inline constexpr uint64_t NewFields=37004000, AssembledFields=111012000, OldFields=74008000;
inline constexpr uint64_t NewDenseBytes=74008000, PhiFields=5898240, LossFields=5406720, ChoiceFields=112640;
inline constexpr uint64_t Graph=3036222110255734598ull;
inline constexpr std::array<uint16_t,Added> AddedIDs{{4,9,13,14,154,258,259,260,326,397,403,414,421,430,447,490}};
static_assert(uint64_t(Sources)*Cells*Added==NewFields && NewFields*2==NewDenseBytes,"exact new16 product");
static_assert(uint64_t(Sources)*Cells*Basis==AssembledFields && uint64_t(Sources)*Cells*Old==OldFields,"exact full110 products");
static_assert(uint64_t(Basis)*Groups*Targets==PhiFields && uint64_t(Sources)*4*Basis*Targets==LossFields,"exact phi/FIT products");

// Root exports this typed object only after the completed full110 fresh proof.
// Runtime compares exact capsule/header bytes, rather than parsing mutable JSON.
struct FrameBinding {
    uint32_t bytes=0,xz_bytes=0,bitmap_bytes=0,finite=0,new_crc=0,assembled_crc=0,shared_old_crc=0;
    uint32_t old_full_crc=0,base_crc=0,storage_phi_crc=0,storage_choice_crc=0;
    int32_t residual_min=0,residual_max=0;
    std::string sha256,authoritative_npy_sha256;
    uint64_t authoritative_npy_bytes=0;
    uint32_t authoritative_npy_data_offset=0;
    uint32_t authoritative_raw_crc=0;
};
struct Binding {
    uint64_t capsule_bytes=0;
    uint32_t header_bytes=0,map_bytes=0,map_crc=0,phi_crc=0,old_phi_crc=0;
    std::string capsule_sha256,header_sha256,map_sha256,map_raw_sha256,pack_sha256;
    std::string completed_full110_sha256,fresh_sha256,generation_frozen_sha256;
    std::string fresh_resource_sha256,parent_build_sha256,parent_raw_proof_sha256;
    uint64_t verified_new_fields=0,verified_assembled_fields=0,verified_old_fields=0;
    uint64_t verified_phi_fields=0,verified_fit_fields=0,verified_choices=0;
    std::array<uint16_t,Sources> sources{};
    std::array<uint16_t,Targets> targets{};
    std::array<uint16_t,Old> old_portIDs{};
    std::array<uint8_t,Targets> old_assignment{};
    std::array<uint16_t,Basis> basis_portIDs{},basis_target_rows{};
    std::array<FrameBinding,Sources> frames{};
};
enum class Status : uint8_t {Unprepared,Ready,NotEligible,OutsideDomain,UnknownPort,ParentMissing,ChosenMissing};
struct BasisRaw {uint16_t value=65535;}; // Exact original medoid cost; no phi/beta/extra/Gap.
struct ProjectedRaw {uint32_t value=0xffffffffu;}; // max(0, exact medoid cost + selected->target phi), once.
}
