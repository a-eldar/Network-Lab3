# Filename: connect-mlx.ps1

param(
    [Parameter(Mandatory=$true)]
    [string]$NodeNumber
)

# Construct the hostname
$TargetHost = "eldar04@mlx-stud-0$NodeNumber"

# Run the SSH command with a jump host
ssh -J eldar04@bava.cs.huji.ac.il $TargetHost