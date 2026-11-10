//%attributes = {"invisible":true,"preemptive":"capable"}
#DECLARE($ref : Integer; $timeout : Real) : Object

// Poll a speech until it is finished or failed, or $timeout seconds have passed.

var $status : Object
var $deadline : Real
$deadline:=Milliseconds+($timeout*1000)

Repeat 
	$status:=Phonon Get status($ref)
	If ($status.state=Phonon state finished) || ($status.state=Phonon state error) || ($status.state=Phonon state unknown)
		return $status
	End if 
	DELAY PROCESS(Current process; 3)
Until (Milliseconds>$deadline)

return $status
