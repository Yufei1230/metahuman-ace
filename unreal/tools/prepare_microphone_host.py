"""Run with UE's Python commandlet, only against the isolated MicrophoneDiagnostics host."""
from pathlib import Path
import unreal

repo = Path(__file__).resolve().parents[2]
host = repo / 'unreal/outputs/MicrophoneDiagnostics'
assert Path(unreal.Paths.project_dir()).resolve() == host.resolve(), 'Use the isolated MicrophoneDiagnostics host'

for map_name, microphone_mode in [('WavConversationTest', False), ('MicrophoneConversationTest', True)]:
    world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.ConversationAudioTestActor, unreal.Vector(0, 0, 0))
    actor.set_actor_label('Conversation Microphone Test' if microphone_mode else 'Conversation WAV Test')
    actor.set_editor_property('auto_start', True)
    actor.set_editor_property('microphone_mode', microphone_mode)
    actor.set_editor_property('show_microphone_controls', microphone_mode)
    actor.set_editor_property('input_wav_path', str(repo / 'services/orchestrator/outputs/demo-input.wav'))
    assert not actor.get_editor_property('is_spatially_loaded')
    assert unreal.EditorLoadingAndSavingUtils.save_map(world, '/Game/' + map_name)
    unreal.log('CONVERSATION_MAP_READY /Game/' + map_name)

config = host / 'Config/DefaultEngine.ini'
config.parent.mkdir(parents=True, exist_ok=True)
previous = config.read_text(encoding='utf-8-sig') if config.exists() else ''
section = '[/Script/EngineSettings.GameMapsSettings]'
if section not in previous:
    config.write_text(previous + '\n' + section + '\nEditorStartupMap=/Game/MicrophoneConversationTest\nGameDefaultMap=/Game/MicrophoneConversationTest\n', encoding='utf-8')
