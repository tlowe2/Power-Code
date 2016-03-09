#include <msp430.h>

void SetVcoreUp (unsigned int level);

/*
 * main.c
 */

unsigned int ADC_Result[64]; //A1 is evens, A0 is odds

void main(void) {
	unsigned int i,j;
	unsigned int ADC_Result_sum;
	unsigned int ADC_Result_Average[2];

    WDTCTL = WDTPW | WDTHOLD;	// Stop watchdog timer

    P1SEL |= BIT6;				// Set P1.6 to output direction (Timer D0.0 output)
    P1DIR |= BIT6;
    P1SEL |= BIT7;				// Set P1.7 to output direction (Timer D0.1 output)
    P1DIR |= BIT7;
    P2SEL |= BIT0;				// Set P2.0 to output direction (Timer D0.2 output)
    P2DIR |= BIT0;
    P1DIR |= 0x01;				// Set P1.0 to output direction (to drive LED)
    P1OUT |= 0x01;				// Set P1.0  - turn LED on
    __delay_cycles(500000);
    P1OUT ^= 0x01;				// Toggle P1.0 using exclusive-or function  - turn LED off

    // Increase Vcore setting to level3 to support fsystem=25MHz
    // NOTE: Change core voltage one level at a time..
    SetVcoreUp (0x01);
    SetVcoreUp (0x02);
    SetVcoreUp (0x03);

    // Initialize DCO to 25MHz
    __bis_SR_register(SCG0);                  // Disable the FLL control loop
    UCSCTL0 = 0x0000;                         // Set lowest possible DCOx, MODx
    UCSCTL1 = DCORSEL_6;                      // Select DCO range 4.6MHz-88MHz operation
    UCSCTL2 = FLLD_1 + 763;                   // Set DCO Multiplier for 25MHz
                                              // (N + 1) * FLLRef = Fdco
                                              // (762 + 1) * 32768 = 25MHz
                                              // Set FLL Div = fDCOCLK/2
    __bic_SR_register(SCG0);                  // Enable the FLL control loop

    // Worst-case settling time for the DCO when the DCO range bits have been
    // changed is n x 32 x 32 x f_MCLK / f_FLL_reference. See UCS chapter in 5xx
    // User Guide for optimization.
    // 32 x 32 x 25 MHz / 32,768 Hz = 782000 = MCLK cycles for DCO to settle
    __delay_cycles(782000);

     // Configure TimerD in Hi-Res Regulated Mode
    TD0CTL0 = TDSSEL_2;                       // TDCLK=SMCLK=25MHz=Hi-Res input clk select
    TD0CTL1 |= TDCLKM_1;                      // Select Hi-res local clock
    TD0HCTL1 |= TDHCLKCR;					  // High-res clock input >15MHz
    TD0HCTL0 = TDHM_0 + 					  // Hi-res clock 8x TDCLK = 200MHz
    		   TDHREGEN + 					  // Regulated mode, locked to input clock
    		   TDHEN;     					  // Hi-res enable

    // Wait some, allow hi-res clock to lock
    P1OUT ^= 0x01;							  // Toggle P1.0 using exclusive-OR, turn LED on
    // __delay_cycles(500000);
    while(!TDHLKIFG);					      // Wait until hi-res clock is locked
    P1OUT ^= 0x01;							  // Toggle P1.0 using exclusive-OR, turn LED off

    // Configure the CCRx blocks
    TD0CCR0 = 2000;                           // PWM Period. So sw freq = 200MHz/2000 = 100 kHz
    TD0CCTL1 = OUTMOD_7 + CLLD_1;             // CCR1 reset/set
    TD0CCR1 = 1550;                           // CCR1 PWM duty cycle of 1900/2000 = 95%
    TD0CCTL2 = OUTMOD_7 + CLLD_1;             // CCR2 reset/set
    TD0CCR2 = 1000;                            // CCR2 PWM duty cycle of 1000/2000 = 50%
    TD0CTL0 |= MC_1 + TDCLR;                  // up-mode, clear TDR, Start timer

    // Configure ADC10 - pulse sample mode; software trigger;
    ADC10CTL0 = ADC10SHT_2 + ADC10ON + ADC10MSC; // 16ADCclks, ADC on
    ADC10CTL1 = ADC10SHP + ADC10CONSEQ_3;     // Sampling timer, rpt seq of ch
    ADC10CTL2 = ADC10RES;                     // 10-bits of resolution
    ADC10MCTL0 = ADC10INCH_1;                 // AVCC ref, A1

    // Configure DMA (ADC10IFG trigger)
    DMACTL0 = DMA0TSEL_24;                    // ADC10IFG trigger
    __data16_write_addr((unsigned short) &DMA0SA,(unsigned long) &ADC10MEM0);
                                              // Source single address
    DMA0SZ = 0x02;                              // 64 conversions
    DMA0CTL = DMADT_4 + DMADSTINCR_3 + DMAEN + DMAIE; // Rpt, inc dest, word access,
                                              // enable int after 64 conversions

    for (;;) {									// Infinite loop, blink LED

    	for(i=0;i<32;i++){
          __data16_write_addr((unsigned short) &DMA0DA,(unsigned long) &ADC_Result[i*2]);
                                            // Update destination array address         
          while (ADC10CTL1 & BUSY);           // Wait if ADC10 core is active
          ADC10CTL0 |= ADC10ENC + ADC10SC;    // Sampling and conversion ready
          __no_operation();                   // BREAKPOINT; check ADC_Result

    	}
    	for(j=0;j<2;j++){
    		ADC_Result_sum = 0x0;                   // clear accumulate register
    		for(i=0;i<64;i=i+2){
    			ADC_Result_sum += ADC_Result[i+j];
    		}
    		ADC_Result_Average[j] = ADC_Result_sum>>5; // Average of 32 conversions resultsads
    	}

    	//TD0CCR1 = ADC_Result_Average + 500;

    __delay_cycles(50000);                   // delay before next 64 conversions
    __no_operation();                   // BREAKPOINT; check ADC_Result
    }
}

#pragma vector=DMA_VECTOR
__interrupt void DMA0_ISR (void)
{
  switch(__even_in_range(DMAIV,16))
  {
    case  0: break;                          // No interrupt
    case  2:
      // 64 conversions complete
    	ADC10CTL0 &= ~ADC10ENC;
    	break;                                 // DMA0IFG
    case  4: break;                          // DMA1IFG
    case  6: break;                          // DMA2IFG
    case  8: break;                          // Reserved
    case 10: break;                          // Reserved
    case 12: break;                          // Reserved
    case 14: break;                          // Reserved
    case 16: break;                          // Reserved
    default: break;
  }
}


void SetVcoreUp (unsigned int level)
  {
  	// Subroutine to change core voltage
    // Open PMM registers for write
    PMMCTL0_H = PMMPW_H;
    // Set SVS/SVM high side new level
    SVSMHCTL = SVSHE + SVSHRVL0 * level + SVMHE + SVSMHRRL0 * level;
    // Set SVM low side to new level
    SVSMLCTL = SVSLE + SVMLE + SVSMLRRL0 * level;
    // Wait till SVM is settled
    while ((PMMIFG & SVSMLDLYIFG) == 0);
    // Clear already set flags
    PMMIFG &= ~(SVMLVLRIFG + SVMLIFG);
    // Set VCore to new level
    PMMCTL0_L = PMMCOREV0 * level;
    // Wait till new level reached
    if ((PMMIFG & SVMLIFG))
      while ((PMMIFG & SVMLVLRIFG) == 0);
    // Set SVS/SVM low side to new level
    SVSMLCTL = SVSLE + SVSLRVL0 * level + SVMLE + SVSMLRRL0 * level;
    // Lock PMM registers for write access
    PMMCTL0_H = 0x00;
}
