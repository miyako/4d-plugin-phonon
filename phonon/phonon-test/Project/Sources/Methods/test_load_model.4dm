//%attributes = {"invisible":true,"preemptive":"capable"}
var $path : Text
$path:=phonon_test_model.path

// failures return 0 and leave a message
ASSERT(Phonon Load model($path+"no-such-folder"; {})=0)
ASSERT(Phonon Get last error#"")
ASSERT(Phonon Load model($path; {language: "klingon"})=0)
ASSERT(Phonon Get last error#"")
ASSERT(Phonon Load model($path; {voice: "nobody"})=0)
ASSERT(Phonon Get last error="@nobody@"; Phonon Get last error)

var $model : Integer
$model:=Phonon Load model($path; {language: "en"})
ASSERT($model>0; Phonon Get last error)
ASSERT(Phonon Get last error="")

var $info : Object
$info:=Phonon Get model info($model)
ASSERT($info.ref=$model)
ASSERT($info.path=$path)
ASSERT($info.sampleRate=24000)
ASSERT($info.language="en")
ASSERT($info.voices.indexOf("default")#-1)
ASSERT($info.languages.length>0)
ASSERT($info.weights="q8_0")

var $models : Collection
$models:=Phonon Get models
ASSERT($models.query("ref = :1"; $model).length=1)

// a voice file added at run time
Phonon ADD VOICE($model; "copy"; $path+"default-voice.safetensors")
ASSERT(Phonon Get last error=""; Phonon Get last error)
ASSERT(Phonon Get model info($model).voices.indexOf("copy")#-1)
Phonon ADD VOICE($model; "missing"; $path+"missing.safetensors")
ASSERT(Phonon Get last error#"")

/*
	On macOS, a system (HFS) path is accepted too.
*/
If (Is macOS)
	var $hfs : Integer
	$hfs:=Phonon Load model(phonon_test_model.platformPath; {})
	ASSERT($hfs>0; Phonon Get last error)
	Phonon UNLOAD MODEL($hfs)
	ASSERT(Phonon Get last error="")
End if 

Phonon UNLOAD MODEL($model)
ASSERT(Phonon Get last error="")
ASSERT(Phonon Get model info($model).ref=Null)
ASSERT(Phonon Get last error#"")
Phonon UNLOAD MODEL($model)
ASSERT(Phonon Get last error#"")
