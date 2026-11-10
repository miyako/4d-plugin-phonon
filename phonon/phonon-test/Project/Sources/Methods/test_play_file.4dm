//%attributes = {"invisible":true,"preemptive":"capable"}
var $model : Integer
$model:=Phonon Load model(phonon_test_model.path; {})

var $file : 4D.File
$file:=Folder(Temporary folder; fk platform path).file("phonon-test.wav")
If ($file.exists)
	$file.delete()
End if 

var $speech : Integer
$speech:=Phonon Play($model; "Saved to a file."; {output: Phonon output none; file: $file.path})
ASSERT(phonon_test_wait($speech; 60).state=Phonon state finished)
ASSERT($file.exists)
ASSERT($file.size>44)
Phonon STOP($speech)

// a bad voice fails the speech, not the call
$speech:=Phonon Play($model; "Hello."; {output: Phonon output none; voice: "nobody"})
ASSERT($speech>0)
var $status : Object
$status:=phonon_test_wait($speech; 60)
ASSERT($status.state=Phonon state error)
ASSERT($status.error="@nobody@")
Phonon STOP($speech)

// bad options fail the call
ASSERT(Phonon Play($model; "Hello."; {output: "speakers"})=0)
ASSERT(Phonon Play($model; "Hello."; {volume: 20})=0)

Phonon UNLOAD MODEL($model)
