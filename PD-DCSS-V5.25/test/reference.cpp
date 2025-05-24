int16_t Encoder_Get(void){
	int16_t Temp;
	Temp = Encoder_Count;
	Encoder_Count = 0;
	
	return Temp;
}

void EXTI0_IRQHandler(void){
	uint32_t EXTI_Line = EXTI_Line0;
	uint16_t GPIO_PinA = GPIO_Pin_0;
	uint16_t GPIO_PinB = GPIO_Pin_1;
	GPIO_TypeDef* GPIOx = GPIOB;
	
	if(EXTI_GetITStatus(EXTI_Line) == SET){
		
		if(GPIO_ReadInputDataBit(GPIOx, GPIO_PinB) == 0){
			Encoder_Count --;
		}
		
		EXTI_ClearITPendingBit(EXTI_Line);
	}
}

void EXTI1_IRQHandler(void){
	uint32_t EXTI_Line = EXTI_Line1;
	uint16_t GPIO_PinA = GPIO_Pin_1;
	uint16_t GPIO_PinB = GPIO_Pin_0;
	GPIO_TypeDef* GPIOx = GPIOB;
	
	if(EXTI_GetITStatus(EXTI_Line) == SET){
		
		if(GPIO_ReadInputDataBit(GPIOx, GPIO_PinB) == 0){
			Encoder_Count ++;
		}
		
		EXTI_ClearITPendingBit(EXTI_Line);
	}
}