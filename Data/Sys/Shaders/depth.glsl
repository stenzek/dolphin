[configuration]
RequiresDepthBuffer=1

[OptionRangeFloat]
GUIName=Minimum Intensity
OptionName=base
MinValue=0
MaxValue=1
StepAmount=0.05
DefaultValue=0.25

[OptionRangeFloat]
GUIName=Intensity Multiplier
OptionName=multiplier
MinValue=1
MaxValue=32
StepAmount=0.5
DefaultValue=16

[/configuration]

void main()
{
	/* inverted depth */
	float depth = 1.0f - SampleDepth();
	float intensity = max(0.0f, min(1.0f, (depth * option_multiplier + option_base)));
	SetOutput(float4(intensity.rrr, 1.0f));
}
