//%attributes = {"invisible":true,"preemptive":"capable"}
var $path : Text
$path:=phonon_test_model.path

var $refs : Collection
$refs:=Phonon Load models([$path; {path: $path; language: "en"; temperature: 0.5}; $path+"no-such-folder"])
ASSERT($refs.length=3)
ASSERT($refs[0]>0)
ASSERT($refs[1]>0)
ASSERT($refs[0]#$refs[1])
ASSERT($refs[2]=0)
ASSERT(Phonon Get last error="#2: @")

var $info : Object
$info:=Phonon Get info
ASSERT($info.models>=2)

Phonon UNLOAD MODEL($refs[0])
Phonon UNLOAD MODEL($refs[1])
var $models : Collection
$models:=Phonon Get models
ASSERT($models.query("ref in :1"; $refs).length=0)
