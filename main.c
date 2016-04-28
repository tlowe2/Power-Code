#include <msp430.h>

void SetVcoreUp (unsigned int level);
int Perturb (int dir);
void adcRead(void);
int Sweep (void);

/*
 * main.c
 *
 * Teddy Lowe and Mike Batbayar, 9/3/2016
 *
 */

// MPPT tuning parameters
#define BATvMAX             350                 // 14.0V with our voltage sense circuit
#define OVERCHARGE_WAIT     10000000            // Large number used to wait to check if charged
#define INITIAL_PWM         1460                // Initial Duty cycle when buck starts 1400/2000 = 60%
#define SWEEPTIME           1500                // Sweep time
#define SWEEPSTART          500	                // Beginning of sweep
#define PERTURBTIME         1000000	            // Time between perturb/observe cycles
#define DELTA_D             5                   // Change between duty cycles for perturb

// Inverter tuning parameters
#define INVERTCOUNT         517                 // This makes our inverter output = 60Hz
#define INVDUTY             350                 // This is how long the modified sine is zero for

volatile int positive_toggle = 0;               // Toggles positive or negative side in timer
volatile int count = 0;                         // Counter variable for timer interrupt

unsigned int ADC_Result[64];                    // A1 is evens, A0 is odds
volatile unsigned int voltage, current;

void main(void) {
    //unsigned int i;
    int direction = 1;                // 1 is up, 0 is down
    unsigned int prev_I = 0;
    unsigned int this_I = 0;
    int current_duty = INITIAL_PWM;
    int sweepcount = SWEEPTIME;

    WDTCTL = WDTPW | WDTHOLD;	// Stop watchdog timer

    P1SEL |= BIT6;				// Set P1.6 to output direction (Timer D0.0 output)
    P1DIR |= BIT6;
    P1SEL |= BIT7;				// Set P1.7 to output direction (Timer D0.1 output)
    P1DIR |= BIT7;
    P2SEL |= BIT0;				// Set P2.0 to output direction (Timer D0.2 output)
    P2DIR |= BIT0;

    // Setup inverter pinouts
    P3DIR |= BIT3;                            // 3.3 for positive side
    P3DIR |= BIT5;                            // 3.5 for negative side

    P3OUT |= BIT3;                            // Start with 3.3 on
    P3OUT &= ~BIT5;                           // Start with 3.5 off

    __delay_cycles(500000);

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
    __delay_cycles(500000);
    while(!TDHLKIFG);					      // Wait until hi-res clock is locked

    // Configure the CCRx blocks
    TD0CCR0 = 2000;                           // PWM Period. So sw freq = 200MHz/2000 = 100 kHz
    TD0CCTL1 = OUTMOD_7 + CLLD_1;             // CCR1 reset/set
    TD0CCR1 = current_duty;                    // CCR1 BUCK initial PWM duty cycle
    TD0CCTL2 = OUTMOD_7 + CLLD_1;             // CCR2 reset/set
    TD0CCR2 = 1000;                           // CCR2 PWM duty cycle of 1000/2000 = 50%
    TD0CTL0 |= MC_1 + TDCLR;                  // up-mode, clear TDR, Start timer

    // Configure ADC10; pulse sample mode, s/w trigger, rpt seq of channels
    ADC10CTL0 = ADC10SHT_2 + ADC10MSC + ADC10ON;  // 16ADCclks, ADC on
    ADC10CTL1 = ADC10SHP + ADC10CONSEQ_3;     // Sampling timer, rpt seq of ch
    ADC10CTL2 = ADC10RES;                     // 10-bit resolution
    ADC10MCTL0 = ADC10INCH_1;                 // AVCC ref, A0, A1(EOS)
  
    // Configure DMA0 (ADC10IFG trigger)
    DMACTL0 = DMA0TSEL_24;                    // ADC10IFG trigger
    __data16_write_addr((unsigned short) &DMA0SA,(unsigned long) &ADC10MEM0);
                                            // Source single address  
    DMA0SZ = 0x02;                            // 2x32 conversions 
    DMA0CTL = DMADT_4 + DMADSTINCR_3 + DMAEN + DMAIE; 
                                            // Rpt, inc dest, byte access, 
   
    // Inverter timer setup using Timer A0
    TA0CCTL0 = CCIE;                          // CCR0 interrupt enabled
    TA0CCR0 = 400;                             // 10 / 1.045 MHz = 9.5us
    TA0CTL = TASSEL_2 + MC_1 + TACLR;         // SMCLK, upmode, clear TAR



    // Main MPPT loop                                         
    while(1) {									// Infinite loop, MPPT

        adcRead();
/*
        if (voltage >= BATvMAX)
        {
            //TD0CCR1 = 0;
            //for (i = 0; i <= OVERCHARGE_WAIT; i++);
            __no_operation();                   // BREAKPOINT

        }
        else
        {
            prev_I = this_I;
            this_I = current;
            
            if (prev_I > this_I)
            {
                direction = !direction;
            }

            current_duty = Perturb(direction);
        }

        sweepcount++;
        if (sweepcount >= SWEEPTIME)
        {
            current_duty = Sweep();
            TD0CCR1 = current_duty;
            sweepcount = 0;
        }
*/
        __no_operation();                   // BREAKPOINT
    }
}

#pragma vector=DMA_VECTOR
__interrupt void DMA0_ISR (void)
{
  switch(__even_in_range(DMAIV,16))
  {
    case  0: break;                          // No interrupt
    case  2: 
        // sequence of conversions complete
        ADC10CTL0 &= ~ADC10ENC;                // Disable ADC conversion
        __bic_SR_register_on_exit(CPUOFF);     // exit LPM
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

// Timer0 A0 interrupt service routine
#pragma vector=TIMER0_A0_VECTOR
__interrupt void TIMER0_A0_ISR(void)
{
  if (count > INVERTCOUNT){
    count = 0;
    positive_toggle = !positive_toggle;
    if (positive_toggle)
      P3OUT ^= BIT5;                            // Toggle P3.5
    else
      P3OUT ^= BIT3;                            // Toggle P3.3
  }else{
    if(count == INVDUTY){
      if (positive_toggle)
        P3OUT ^= BIT5;
      else
        P3OUT ^= BIT3;
    }
  }
  count++;
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

void adcRead (void)
{
    unsigned int i,j;
    unsigned int ADC_Result_sum;
    unsigned int ADC_Result_Average[2];

    for(i=0;i<32;i++)
    {
        __data16_write_addr((unsigned short) &DMA0DA,(unsigned long) &ADC_Result[i*2]);
                                                // Update destination array address         
        while (ADC10CTL1 & BUSY);           // Wait if ADC10 core is active
        ADC10CTL0 |= ADC10ENC + ADC10SC;    // Sampling and conversion ready
        __bis_SR_register(CPUOFF + GIE);    // LPM0, ADC10_ISR will force exit

    }

    for(j=0;j<2;j++)
    {
          
        ADC_Result_sum = 0x0;                       // clear accumulate register
           
        for(i=0;i<64;i=i+2)
        {
            ADC_Result_sum += ADC_Result[i+j];       // Alternate even/odd elements
        }

        ADC_Result_Average[j] = ADC_Result_sum>>5;    // Average of 32 conversions resultsads
    }

    voltage = ADC_Result_Average[0];
    current = ADC_Result_Average[1];
}

int Perturb (int dir)
{
    int buff;
    //Subroutine to change duty cycle
    if (dir > 1 || dir < 0)
    {
        return -1;
    }
    buff = TD0CCR1;
    
    if (dir)
    {
        buff = buff + DELTA_D;
        if (buff > 2000)
        {
            buff = 2000;
        }

        TD0CCR1 = buff;
    }
    else if (!dir)
    {
        buff = buff - DELTA_D;
        if (buff < SWEEPSTART)
        {
            buff = SWEEPSTART;
        }
        TD0CCR1 = buff;
    }
    
    __delay_cycles(PERTURBTIME);
    return buff;
}

int Sweep (void)
{
    int duty = SWEEPSTART;
    int bestduty = 0;
    int bestcurrent = 0;
    unsigned int lastcurrent = 0;
    int currenthold[2];
    TD0CCR1 = duty;

    while (duty < 2000)
    {
        adcRead();
        currenthold[0] = current;
        adcRead();
        currenthold[1] = current;

        current = (currenthold[1]+currenthold[0])>>1;

        if (current > lastcurrent)
        {
        	if (current > bestcurrent)
        	{
        		bestcurrent = current;
                bestduty = duty;
                __no_operation();
        	}
        }

        lastcurrent = current;
        duty = Perturb(1);
    }

    return bestduty;
}
