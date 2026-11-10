//%attributes = {"invisible":true,"preemptive":"capable"}
/*
	The default output device, muted. A machine without an audio device
	(such as a CI runner) ends the speech in the error state instead.
*/
var $model : Integer
$model:=Phonon Load model(phonon_test_model.path; {})

var $speech : Integer
$speech:=Phonon Play($model; "Hello."; {volume: 0})
ASSERT($speech>0; Phonon Get last error)

var $status : Object
$status:=phonon_test_wait($speech; 60)
ASSERT([Phonon state finished; Phonon state error].indexOf($status.state)#-1; JSON Stringify($status))
If ($status.state=Phonon state error)
	ASSERT($status.error="audio output: @")
Else 
	ASSERT($status.position>0)
End if 
Phonon STOP($speech)

Phonon UNLOAD MODEL($model)
