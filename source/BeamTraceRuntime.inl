// Temporary: the step at which the shadow of the player's car's beam is lost, written to
// GTAIV-beam-trace.log next to the ini whenever the set of steps it got through changes.
namespace BeamTrace
{
    enum Stage : unsigned
    {
        CarSeen,      // a render phase of this frame took the car in
        LightsMade,   // its lights were made (in view, or OffscreenLights)
        BeamOffered,  // a beam of it reached the headlight shadow selector
        ShadowKept,   // the selector kept its shadow flag
        Candidate,    // the shadow allocation saw it among the lights with a shadow
        Selected,     // the shadow allocation gave it a slot
        StageCount
    };
    static constexpr const char* StageNames[StageCount]{ "seen", "lights", "offered", "kept", "candidate", "selected" };
    static std::array<std::atomic<uint32_t>, StageCount> lastFrame{};
    static std::filesystem::path path;
    static uint32_t lastBits = ~0u;
    static int lines = 0;

    static void Mark(Stage stage) noexcept
    {
        if (!path.empty() && CTimer::m_frameCount)
            lastFrame[stage].store(*CTimer::m_frameCount, std::memory_order_relaxed);
    }

    // Once a game frame, from the process callback.
    static void Update() noexcept
    {
        if (path.empty() || !CTimer::m_frameCount || lines >= 5000) return;
        const uint32_t frame = *CTimer::m_frameCount;
        uint32_t bits = 0;
        for (unsigned stage = 0; stage < StageCount; ++stage)
            if (frame - lastFrame[stage].load(std::memory_order_relaxed) <= 2)
                bits |= 1u << stage;
        const auto car = PlayerCar::Last();
        if (!car) bits = 0;
        if (bits == lastBits) return;
        lastBits = bits;
        float carPos[3]{}, camera[3]{}, ped[3]{};
        CEntity::GetPosition(car, carPos);
        GameCamera::Position(camera);
        if (CPlayer::getLocalPlayerPed) CEntity::GetPosition(CPlayer::getLocalPlayerPed(), ped);
        const auto distance = [](const float* a, const float* b)
            { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); };
        try
        {
            std::ofstream out(path, lines ? std::ios::app : std::ios::trunc);
            out << "frame=" << frame << " tick=" << GetTickCount() << " car=" << (car != 0);
            for (unsigned stage = 0; stage < StageCount; ++stage)
                out << ' ' << StageNames[stage] << '=' << ((bits >> stage) & 1);
            const auto matrix = CEntity::GetMatrix(car);
            out << " car_camera=" << distance(carPos, camera) << " car_player=" << distance(carPos, ped)
                << " in_car=" << (CPlayer::findPlayerCar && CPlayer::findPlayerCar() != 0)
                << " lights_on=" << (car ? int(*reinterpret_cast<const uint8_t*>(car + 0xF15)) : -1) << '\n';
            ++lines;
        }
        catch (...) {}
    }
}
